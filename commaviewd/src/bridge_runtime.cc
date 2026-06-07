#include "framing.h"
#include "socket.h"
#include "policy.h"
#include "router.h"
#include "telemetry_policy.h"
#include "runtime_debug_config.h"
#include "ui_export_socket.h"
/**
 * CommaView Unified Bridge (C++)
 *
 * Streams HEVC video and raw telemetry on legacy road+wide ports, plus a telemetry-only stream.
 *
 * Ports:
 *   8200 road, 8201 wide, 8202 driver, 8203 telemetry
 *
 * Framing:
 *   [4-byte big-endian length][payload]
 *   payload[0] = 0x05 (video): [type][timestamp_ns_be64][width_be32][height_be32][header_len_be32][header][data]
 *   payload[0] = 0x02 (meta):  [type][json bytes]
 *   payload[0] = 0x03 (control inbound): [type][json bytes]
 *   payload[0] = 0x04 (meta-raw): [type][version][service_idx][raw_len_be32][raw_event]
 */
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <atomic>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <array>
#include <sstream>
#include <fstream>

#include <capnp/serialize.h>
#include "cereal/gen/cpp/log.capnp.h"
#include "cereal/messaging/messaging.h"
#include "cereal/services.h"
using commaview::telemetry::ServicePolicy;
using commaview::telemetry::service_policy_samples;
using commaview::runtime_debug::LoadedRuntimeDebugConfig;
using commaview::runtime_debug::effective_runtime_debug_config;
using commaview::runtime_debug::policy_for_service;
using commaview::runtime_debug::render_config_json;
using commaview::runtime_debug::runtime_debug_effective_path;
using commaview::runtime_debug::runtime_debug_stats_path;
using commaview::runtime_debug::service_mode_to_string;
using commaview::telemetry::default_service_policy_for_name;
using commaview::telemetry::service_policy_subscribes;
using commaview::telemetry::telemetry_policy_allows_emit;
using commaview::telemetry::telemetry_policy_fetches_latest;


static constexpr uint8_t MSG_VIDEO = 0x05;
static constexpr uint8_t MSG_CONTROL = 0x03;
static constexpr uint8_t MSG_META_RAW = 0x04;
static constexpr uint8_t RAW_META_ENVELOPE_V4 = 0x04;
static constexpr uint8_t RAW_META_ENVELOPE_V5 = 0x05;

static constexpr int PORT_ROAD = 8200;
static constexpr int PORT_WIDE = 8201;
static constexpr int PORT_DRIVER = 8202;
static constexpr int PORT_TELEMETRY = 8203;
static constexpr uint64_t VIDEO_SEND_BUDGET_MICROS = 16000ULL;

// Runtime policy defaults favor the upstream-organized onroad domains we export over the UI socket.
static constexpr std::array<const char*, 20> kTelemetryServices = {
  "uiStateOnroad",
  "selfdriveState",
  "carState",
  "controlsState",
  "onroadEvents",
  "driverMonitoringState",
  "driverStateV2",
  "modelV2",
  "radarState",
  "liveCalibration",
  "carOutput",
  "carControl",
  "liveParameters",
  "longitudinalPlan",
  "carParams",
  "deviceState",
  "roadCameraState",
  "pandaStatesSummary",
  "onroadProjection",
  "wideRoadCameraState",
};
static constexpr int NUM_TELEM = static_cast<int>(kTelemetryServices.size());
static constexpr int TELEMETRY_EMIT_MS_DEFAULT = 50;  // 20 Hz base poll for PASS and future SAMPLE modes
static int g_telemetry_emit_ms = TELEMETRY_EMIT_MS_DEFAULT;

static const char* VIDEO_SERVICES_PROD[] = {
  "roadEncodeData", "wideRoadEncodeData", "driverEncodeData"
};


static std::atomic<bool> g_running{true};


static std::atomic<int> g_active_road{0};
static std::atomic<int> g_active_wide{0};
static std::atomic<int> g_active_driver{0};
static std::unique_ptr<commaview::ui_export::SocketServer> g_ui_export_socket;

static std::atomic<int>& active_counter_for_port(int port) {
  if (port == PORT_ROAD) return g_active_road;
  if (port == PORT_WIDE) return g_active_wide;
  return g_active_driver;
}

static cereal::Event::Which expected_video_which_for_port(int port) {
  return commaview::video::expected_video_which_for_port(port, false);
}






static size_t queue_size_for_service(const char* service_name) {
  auto it = services.find(std::string(service_name));
  if (it == services.end()) return 0;
  return it->second.queue_size;
}

static void put_be32(uint8_t* buf, uint32_t val) {
  commaview::net::put_be32(buf, val);
}

static void put_be64(uint8_t* buf, uint64_t val) {
  for (int i = 7; i >= 0; --i) {
    buf[7 - i] = static_cast<uint8_t>((val >> (i * 8)) & 0xFF);
  }
}



static void telemetry_loop(int client_fd,
                           const char* stream_name,
                           std::atomic<bool>* disconnect_requested,
                           std::mutex* send_mutex,
                           std::atomic<bool>* telemetry_enabled);

static commaview::net::SendResult send_meta_bytes_bounded(int fd,
                                                           const uint8_t* bytes,
                                                           size_t bytes_len,
                                                           uint8_t msg_type,
                                                           std::mutex* send_mutex) {
  commaview::net::SendResult empty_ok{};
  if (bytes == nullptr || bytes_len == 0) return empty_ok;
  std::vector<uint8_t> framed_payload(1 + bytes_len);
  framed_payload[0] = msg_type;
  memcpy(&framed_payload[1], bytes, bytes_len);
  if (send_mutex != nullptr) {
    std::lock_guard<std::mutex> send_lock(*send_mutex);
    return commaview::net::send_frame_bounded(
        fd,
        framed_payload.data(),
        framed_payload.size(),
        commaview::net::SendDeadline::after_micros(VIDEO_SEND_BUDGET_MICROS));
  }
  return commaview::net::send_frame_bounded(
      fd,
      framed_payload.data(),
      framed_payload.size(),
      commaview::net::SendDeadline::after_micros(VIDEO_SEND_BUDGET_MICROS));
}

static commaview::net::SendResult send_meta_raw_frame(int fd,
                                                       uint8_t envelope_version,
                                                       uint8_t service_index,
                                                       const uint8_t* raw_data,
                                                       size_t raw_size,
                                                       std::mutex* send_mutex) {
  commaview::net::SendResult empty_ok{};
  if (raw_data == nullptr || raw_size == 0) return empty_ok;
  const uint32_t raw_len = static_cast<uint32_t>(raw_size);
  std::vector<uint8_t> payload(1 + 1 + 4 + raw_len);
  payload[0] = envelope_version;
  payload[1] = service_index;
  put_be32(&payload[2], raw_len);
  memcpy(&payload[6], raw_data, raw_len);
  return send_meta_bytes_bounded(fd, payload.data(), payload.size(), MSG_META_RAW, send_mutex);
}

struct RuntimeServiceStats {
  uint64_t active_subscribers = 0;
  uint64_t receive_count = 0;
  uint64_t receive_bytes = 0;
  uint64_t drain_count = 0;
  uint64_t max_drained_burst = 0;
  uint64_t sampled_emit_count = 0;
  uint64_t sampled_emit_bytes = 0;
  uint64_t emitted_count = 0;
  uint64_t emitted_bytes = 0;
  uint64_t last_receive_ms = 0;
  uint64_t last_emit_ms = 0;
  uint64_t send_failure_count = 0;
  uint64_t max_send_stall_micros = 0;
  uint64_t send_backpressure_count = 0;
};

struct RuntimeLoopStats {
  uint64_t iterations = 0;
  uint64_t total_micros = 0;
  uint64_t max_micros = 0;
  uint64_t over_budget = 0;
};

struct RuntimeVideoSendStats {
  uint64_t ok_count = 0;
  uint64_t backpressure_count = 0;
  uint64_t disconnect_count = 0;
  uint64_t partial_reset_count = 0;
  uint64_t zero_byte_drop_count = 0;
  uint64_t max_send_micros = 0;
  std::string last_status = "ok";
  int last_error = 0;
  std::string last_error_name = "none";
  uint64_t last_bytes_sent = 0;
  uint64_t last_elapsed_micros = 0;
  uint64_t last_at_ms = 0;
};

struct RuntimePeerDisconnectStats {
  uint64_t count = 0;
  std::string stream = "";
  std::string phase = "";
  std::string status = "ok";
  int error = 0;
  std::string error_name = "none";
  uint64_t bytes_sent = 0;
  uint64_t elapsed_micros = 0;
  uint64_t at_ms = 0;
};

struct RuntimeState {
  LoadedRuntimeDebugConfig loaded_config = {};
  LoadedRuntimeDebugConfig effective_config = {};
  std::vector<RuntimeServiceStats> services = std::vector<RuntimeServiceStats>(NUM_TELEM);
  RuntimeLoopStats telemetry_loop = {};
  RuntimeLoopStats video_loop = {};
  RuntimeVideoSendStats video_send = {};
  RuntimePeerDisconnectStats peer_disconnect = {};
  std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
  uint64_t reconnect_count = 0;
  std::string last_restart_reason = "startup";
  bool initialized = false;
};

static std::mutex g_runtime_state_mutex;
static RuntimeState g_runtime_state;

static uint64_t runtime_now_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

static std::string runtime_json_escape(const std::string& in) {
  return commaview::runtime_debug::json_escape(in);
}

static uint64_t runtime_loop_avg_micros(const RuntimeLoopStats& loop) {
  return loop.iterations == 0 ? 0 : (loop.total_micros / loop.iterations);
}

static void note_runtime_loop(RuntimeLoopStats* loop, uint64_t elapsed_micros, int budget_ms) {
  if (loop == nullptr) return;
  loop->iterations += 1;
  loop->total_micros += elapsed_micros;
  loop->max_micros = std::max(loop->max_micros, elapsed_micros);
  const uint64_t budget_micros = budget_ms > 0 ? static_cast<uint64_t>(budget_ms) * 1000ULL : 0ULL;
  if (budget_micros > 0 && elapsed_micros > budget_micros) loop->over_budget += 1;
}

static void write_text_file_best_effort(const std::string& path, const std::string& body) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) return;
  out << body;
}

static void append_runtime_run_event(const std::string& event,
                                     const std::string& stream = "",
                                     const std::string& peer = "",
                                     const std::string& phase = "",
                                     const std::string& status = "") {
  std::ofstream out("/data/commaview/logs/runtime-run-events.jsonl", std::ios::app);
  if (!out) return;
  std::string restart_reason;
  {
    std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
    restart_reason = g_runtime_state.last_restart_reason;
  }
  out << "{"
      << "\"tsMs\":" << runtime_now_ms() << ","
      << "\"event\":\"" << runtime_json_escape(event) << "\"," 
      << "\"pid\":" << getpid() << ","
      << "\"mode\":\"bridge\"," 
      << "\"restartReason\":\"" << runtime_json_escape(restart_reason) << "\"";
  if (!stream.empty()) out << ",\"stream\":\"" << runtime_json_escape(stream) << "\"";
  if (!peer.empty()) out << ",\"peer\":\"" << runtime_json_escape(peer) << "\"";
  if (!phase.empty()) out << ",\"phase\":\"" << runtime_json_escape(phase) << "\"";
  if (!status.empty()) out << ",\"status\":\"" << runtime_json_escape(status) << "\"";
  out << "}\n";
}

static std::string build_runtime_stats_json_locked() {
  std::ostringstream out;
  const uint64_t uptime_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - g_runtime_state.started_at).count());
  out << "{";
  out << "\"uptimeMs\":" << uptime_ms << ",";
  out << "\"reconnectCount\":" << g_runtime_state.reconnect_count << ",";
  out << "\"configVersion\":" << g_runtime_state.effective_config.config_version << ",";
  out << "\"configHash\":\"" << runtime_json_escape(g_runtime_state.effective_config.config_hash) << "\",";
  out << "\"lastRestartReason\":\"" << runtime_json_escape(g_runtime_state.last_restart_reason) << "\",";
  out << "\"telemetryLoop\":{";
  out << "\"iterations\":" << g_runtime_state.telemetry_loop.iterations << ",";
  out << "\"avgMicros\":" << runtime_loop_avg_micros(g_runtime_state.telemetry_loop) << ",";
  out << "\"maxMicros\":" << g_runtime_state.telemetry_loop.max_micros << ",";
  out << "\"overBudget\":" << g_runtime_state.telemetry_loop.over_budget << "},";
  out << "\"videoLoop\":{";
  out << "\"iterations\":" << g_runtime_state.video_loop.iterations << ",";
  out << "\"avgMicros\":" << runtime_loop_avg_micros(g_runtime_state.video_loop) << ",";
  out << "\"maxMicros\":" << g_runtime_state.video_loop.max_micros << ",";
  out << "\"overBudget\":" << g_runtime_state.video_loop.over_budget << "},";
  out << "\"videoSend\":{";
  out << "\"okCount\":" << g_runtime_state.video_send.ok_count << ",";
  out << "\"backpressureCount\":" << g_runtime_state.video_send.backpressure_count << ",";
  out << "\"disconnectCount\":" << g_runtime_state.video_send.disconnect_count << ",";
  out << "\"partialResetCount\":" << g_runtime_state.video_send.partial_reset_count << ",";
  out << "\"zeroByteDropCount\":" << g_runtime_state.video_send.zero_byte_drop_count << ",";
  out << "\"maxSendMicros\":" << g_runtime_state.video_send.max_send_micros << ",";
  out << "\"lastStatus\":\"" << runtime_json_escape(g_runtime_state.video_send.last_status) << "\",";
  out << "\"lastError\":" << g_runtime_state.video_send.last_error << ",";
  out << "\"lastErrorName\":\"" << runtime_json_escape(g_runtime_state.video_send.last_error_name) << "\",";
  out << "\"lastBytesSent\":" << g_runtime_state.video_send.last_bytes_sent << ",";
  out << "\"lastElapsedMicros\":" << g_runtime_state.video_send.last_elapsed_micros << ",";
  out << "\"lastAtMs\":" << g_runtime_state.video_send.last_at_ms << "},";
  out << "\"peerDisconnect\":{"
      << "\"count\":" << g_runtime_state.peer_disconnect.count << ","
      << "\"stream\":\"" << runtime_json_escape(g_runtime_state.peer_disconnect.stream) << "\","
      << "\"phase\":\"" << runtime_json_escape(g_runtime_state.peer_disconnect.phase) << "\","
      << "\"status\":\"" << runtime_json_escape(g_runtime_state.peer_disconnect.status) << "\","
      << "\"error\":" << g_runtime_state.peer_disconnect.error << ","
      << "\"errorName\":\"" << runtime_json_escape(g_runtime_state.peer_disconnect.error_name) << "\","
      << "\"bytesSent\":" << g_runtime_state.peer_disconnect.bytes_sent << ","
      << "\"elapsedMicros\":" << g_runtime_state.peer_disconnect.elapsed_micros << ","
      << "\"atMs\":" << g_runtime_state.peer_disconnect.at_ms << "},";
  out << "\"services\":{";
  for (int i = 0; i < NUM_TELEM; ++i) {
    if (i > 0) out << ",";
    const char* service_name = kTelemetryServices[static_cast<size_t>(i)];
    const ServicePolicy policy = policy_for_service(g_runtime_state.effective_config, service_name);
    const RuntimeServiceStats& stats = g_runtime_state.services[static_cast<size_t>(i)];
    out << "\"" << service_name << "\":{";
    out << "\"mode\":\"" << service_mode_to_string(policy.mode) << "\",";
    out << "\"subscribed\":" << (stats.active_subscribers > 0 ? "true" : "false") << ",";
    out << "\"sampleHz\":" << policy.sample_hz << ",";
    out << "\"receiveCount\":" << stats.receive_count << ",";
    out << "\"receiveBytes\":" << stats.receive_bytes << ",";
    out << "\"drainCount\":" << stats.drain_count << ",";
    out << "\"maxDrainedBurst\":" << stats.max_drained_burst << ",";
    out << "\"sampledEmitCount\":" << stats.sampled_emit_count << ",";
    out << "\"sampledEmitBytes\":" << stats.sampled_emit_bytes << ",";
    out << "\"emittedCount\":" << stats.emitted_count << ",";
    out << "\"emittedBytes\":" << stats.emitted_bytes << ",";
    out << "\"lastReceiveMs\":" << stats.last_receive_ms << ",";
    out << "\"lastEmitMs\":" << stats.last_emit_ms << ",";
    out << "\"sendFailureCount\":" << stats.send_failure_count << ",";
    out << "\"maxSendStallMicros\":" << stats.max_send_stall_micros << ",";
    out << "\"sendBackpressureCount\":" << stats.send_backpressure_count;
    out << "}";
  }
  out << "}";
  if (g_ui_export_socket != nullptr) {
    const auto socket_stats = g_ui_export_socket->stats();
    out << ",\"uiExportSocket\":{";
    out << "\"running\":" << (socket_stats.running ? "true" : "false") << ",";
    out << "\"connected\":" << (socket_stats.connected ? "true" : "false") << ",";
    out << "\"connectCount\":" << socket_stats.connect_count << ",";
    out << "\"acceptedCount\":" << socket_stats.accepted_count << ",";
    out << "\"malformedCount\":" << socket_stats.malformed_count << ",";
    out << "\"lastReceiveMs\":" << socket_stats.last_receive_ms;
    out << "}";
  }
  out << "}";
  return out.str();
}

struct RuntimeRenderedState {
  std::string effective_json;
  std::string stats_json;
};

static RuntimeRenderedState render_runtime_state_locked() {
  return RuntimeRenderedState{
      render_config_json(g_runtime_state.effective_config, true),
      build_runtime_stats_json_locked(),
  };
}

static void flush_runtime_state() {
  RuntimeRenderedState rendered;
  {
    std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
    rendered = render_runtime_state_locked();
  }
  write_text_file_best_effort(runtime_debug_effective_path(), rendered.effective_json);
  write_text_file_best_effort(runtime_debug_stats_path(), rendered.stats_json);
}

static void initialize_runtime_state_once() {
  {
    std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
    if (g_runtime_state.initialized) return;
    g_runtime_state.loaded_config = commaview::runtime_debug::load_runtime_debug_config();
    g_runtime_state.effective_config = effective_runtime_debug_config(g_runtime_state.loaded_config);
    g_runtime_state.started_at = std::chrono::steady_clock::now();
    const char* restart_reason = std::getenv("COMMAVIEWD_RESTART_REASON");
    if (restart_reason != nullptr && restart_reason[0] != '\0') {
      g_runtime_state.last_restart_reason = restart_reason;
    }
    g_runtime_state.initialized = true;
  }
  flush_runtime_state();
}

static void note_runtime_connect() {
  {
    std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
    g_runtime_state.reconnect_count += 1;
  }
  flush_runtime_state();
}

static constexpr uint64_t kTelemetryEmitBackpressureThresholdMicros = 5000ULL;

static void note_runtime_emit(int idx, size_t bytes, bool sampled, bool ok, uint64_t stall_micros) {
  if (idx < 0 || idx >= NUM_TELEM) return;
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  RuntimeServiceStats& stats = g_runtime_state.services[static_cast<size_t>(idx)];
  stats.max_send_stall_micros = std::max(stats.max_send_stall_micros, stall_micros);
  if (stall_micros > kTelemetryEmitBackpressureThresholdMicros) {
    stats.send_backpressure_count += 1;
  }
  if (!ok) {
    stats.send_failure_count += 1;
    return;
  }
  stats.emitted_count += 1;
  stats.emitted_bytes += static_cast<uint64_t>(bytes);
  stats.last_emit_ms = runtime_now_ms();
  if (sampled) {
    stats.sampled_emit_count += 1;
    stats.sampled_emit_bytes += static_cast<uint64_t>(bytes);
  }
}

static void note_runtime_loop_sample(bool telemetry_loop, uint64_t elapsed_micros) {
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  if (telemetry_loop) {
    note_runtime_loop(&g_runtime_state.telemetry_loop, elapsed_micros, g_telemetry_emit_ms);
  } else {
    note_runtime_loop(&g_runtime_state.video_loop, elapsed_micros, 20);
  }
}

static void note_video_send_failure_details(const commaview::net::SendResult& result) {
  g_runtime_state.video_send.last_status = commaview::net::send_status_name(result.status);
  g_runtime_state.video_send.last_error = result.error;
  g_runtime_state.video_send.last_error_name = commaview::net::send_error_name(result.error);
  g_runtime_state.video_send.last_bytes_sent = static_cast<uint64_t>(result.bytes_sent);
  g_runtime_state.video_send.last_elapsed_micros = result.elapsed_micros;
  g_runtime_state.video_send.last_at_ms = runtime_now_ms();
}

static void note_video_send_result(const commaview::net::SendResult& result) {
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  g_runtime_state.video_send.max_send_micros = std::max(g_runtime_state.video_send.max_send_micros,
                                                        result.elapsed_micros);
  if (result.status != commaview::net::SendStatus::Ok) note_video_send_failure_details(result);
  switch (result.status) {
    case commaview::net::SendStatus::Ok:
      g_runtime_state.video_send.ok_count += 1;
      break;
    case commaview::net::SendStatus::Backpressure:
      g_runtime_state.video_send.backpressure_count += 1;
      if (result.bytes_sent == 0) g_runtime_state.video_send.zero_byte_drop_count += 1;
      if (result.bytes_sent > 0) g_runtime_state.video_send.partial_reset_count += 1;
      break;
    case commaview::net::SendStatus::Disconnected:
    case commaview::net::SendStatus::InvalidArgument:
      g_runtime_state.video_send.disconnect_count += 1;
      break;
  }
}

static commaview::net::SendResult peer_closed_result() {
  commaview::net::SendResult result{};
  result.status = commaview::net::SendStatus::Disconnected;
  return result;
}

static void note_runtime_peer_disconnect(const char* stream,
                                         const char* phase,
                                         const commaview::net::SendResult& result) {
  {
    std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
    g_runtime_state.peer_disconnect.count += 1;
    g_runtime_state.peer_disconnect.stream = stream == nullptr ? "" : stream;
    g_runtime_state.peer_disconnect.phase = phase == nullptr ? "" : phase;
    g_runtime_state.peer_disconnect.status = commaview::net::send_status_name(result.status);
    g_runtime_state.peer_disconnect.error = result.error;
    g_runtime_state.peer_disconnect.error_name = commaview::net::send_error_name(result.error);
    g_runtime_state.peer_disconnect.bytes_sent = static_cast<uint64_t>(result.bytes_sent);
    g_runtime_state.peer_disconnect.elapsed_micros = result.elapsed_micros;
    g_runtime_state.peer_disconnect.at_ms = runtime_now_ms();
  }
  append_runtime_run_event("peer_disconnect",
                           stream == nullptr ? "unknown" : stream,
                           "",
                           phase == nullptr ? "unknown" : phase,
                           commaview::net::send_status_name(result.status));
  printf("[%s] peer disconnect phase=%s status=%s errno=%s(%d) bytes=%zu elapsed_us=%llu\n",
         stream == nullptr ? "unknown" : stream,
         phase == nullptr ? "unknown" : phase,
         commaview::net::send_status_name(result.status),
         commaview::net::send_error_name(result.error).c_str(),
         result.error,
         result.bytes_sent,
         static_cast<unsigned long long>(result.elapsed_micros));
  fflush(stdout);
  flush_runtime_state();
}

static void set_session_policy(const std::string& session_id, bool suppress_video) {
  commaview::control::set_session_policy(session_id, suppress_video);
}

static bool get_session_policy(const std::string& session_id, bool* suppress_video) {
  return commaview::control::get_session_policy(session_id, suppress_video);
}

using ClientControlState = commaview::control::ClientControlState;

static void consume_client_control_frames(int client_fd,
                                          ClientControlState* state,
                                          const char* video_service) {
  commaview::control::consume_client_control_frames(client_fd, state, video_service, MSG_CONTROL);
}

static commaview::net::SendResult send_frame_locked(int fd,
                                                    const uint8_t* payload,
                                                    size_t payload_len,
                                                    std::mutex* send_mutex) {
  if (send_mutex != nullptr) {
    std::lock_guard<std::mutex> send_lock(*send_mutex);
    return commaview::net::send_frame_bounded(
        fd,
        payload,
        payload_len,
        commaview::net::SendDeadline::after_micros(VIDEO_SEND_BUDGET_MICROS));
  }
  return commaview::net::send_frame_bounded(
      fd,
      payload,
      payload_len,
      commaview::net::SendDeadline::after_micros(VIDEO_SEND_BUDGET_MICROS));
}

static commaview::net::SendResult send_buffers_locked(int fd,
                                                      const commaview::net::SendBuffer* buffers,
                                                      size_t buffer_count,
                                                      std::mutex* send_mutex) {
  if (send_mutex != nullptr) {
    std::lock_guard<std::mutex> send_lock(*send_mutex);
    return commaview::net::send_buffers_bounded(
        fd,
        buffers,
        buffer_count,
        commaview::net::SendDeadline::after_micros(VIDEO_SEND_BUDGET_MICROS));
  }
  return commaview::net::send_buffers_bounded(
      fd,
      buffers,
      buffer_count,
      commaview::net::SendDeadline::after_micros(VIDEO_SEND_BUDGET_MICROS));
}

static bool client_socket_alive(int fd) {
  return commaview::net::client_socket_alive(fd);
}

static int create_server(int port) {
  return commaview::net::create_server(port);
}

static cereal::EncodeData::Reader read_encode_data(cereal::Event::Reader event, int port) {
  return commaview::video::read_encode_data(event, port, false);
}

static void handle_video_client(int client_fd, const char* video_service, int port) {
  char addr_str[64] = {};
  struct sockaddr_in peer = {};
  socklen_t peer_len = sizeof(peer);
  if (getpeername(client_fd, reinterpret_cast<struct sockaddr*>(&peer), &peer_len) == 0) {
    inet_ntop(AF_INET, &peer.sin_addr, addr_str, sizeof(addr_str));
  }

  append_runtime_run_event("client_connected", video_service, addr_str);
  printf("[%s] client connected: %s (fd=%d)\n", video_service, addr_str, client_fd);
  fflush(stdout);

  int opt = 1;
  setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  int sndbuf = 524288;
  setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  auto& active_counter = active_counter_for_port(port);
  int previous = active_counter.fetch_add(1);
  if (previous > 0) {
    active_counter.fetch_sub(1);
    close(client_fd);
    return;
  }

  note_runtime_connect();

  Context* ctx = Context::create();

  const bool enable_video = true;
  SubSocket* video_sock = nullptr;
  if (enable_video) {
    const size_t video_segment_size = queue_size_for_service(video_service);
    video_sock = SubSocket::create(ctx, video_service, "127.0.0.1", true, true, video_segment_size);
  }

  const bool include_telemetry = (port == PORT_ROAD || port == PORT_WIDE);

  Poller* video_poller = Poller::create();
  if (video_sock != nullptr) video_poller->registerSocket(video_sock);

  std::atomic<bool> telemetry_disconnect_requested{false};
  std::atomic<bool> telemetry_enabled_for_client{include_telemetry};
  std::thread telemetry_thread;
  std::mutex send_mutex;
  if (include_telemetry) {
    telemetry_thread = std::thread(telemetry_loop,
                                   client_fd,
                                   video_service,
                                   &telemetry_disconnect_requested,
                                   &send_mutex,
                                   &telemetry_enabled_for_client);
  }

  uint64_t frame_count = 0;
  uint64_t parse_error_count = 0;
  uint64_t wrong_union_count = 0;
  uint64_t suppressed_video_count = 0;
  auto t0 = std::chrono::steady_clock::now();
  AlignedBuffer aligned_buf;
  ClientControlState control_state;

  while (g_running) {
    const auto loop_started = std::chrono::steady_clock::now();
    if (!client_socket_alive(client_fd)) {
      note_runtime_peer_disconnect(video_service, "client_socket_alive", peer_closed_result());
      break;
    }

    consume_client_control_frames(client_fd, &control_state, video_service);
    if (include_telemetry) {
      telemetry_enabled_for_client.store(control_state.telemetry_on_video);
    }

    if (include_telemetry && telemetry_disconnect_requested.load()) break;

    int video_poll_ms = 20;
    auto ready = video_poller->poll(video_poll_ms);

    for (auto* sock : ready) {
      std::unique_ptr<Message> raw_msg(sock->receive(true));
      if (!raw_msg) continue;
      const size_t raw_size = raw_msg->getSize();

      try {
        capnp::ReaderOptions options;
        options.traversalLimitInWords = kj::maxValue;

        capnp::FlatArrayMessageReader reader(aligned_buf.align(raw_msg.get()), options);
        auto event = reader.getRoot<cereal::Event>();

        if (video_sock != nullptr && sock == video_sock) {
          const auto which = event.which();
          const auto expected = expected_video_which_for_port(port);
          if (which != expected) {
            wrong_union_count++;
            if (wrong_union_count <= 20 || (wrong_union_count % 100) == 0) {
              printf("[%s] union mismatch #%llu: got=%d expected=%d raw=%zu\n",
                     video_service,
                     static_cast<unsigned long long>(wrong_union_count),
                     static_cast<int>(which),
                     static_cast<int>(expected),
                     raw_size);
              fflush(stdout);
            }
            continue;
          }

          auto ed = read_encode_data(event, port);
          auto header = ed.getHeader();
          auto data = ed.getData();

          const uint32_t header_len = header.size();
          const size_t data_len = data.size();
          const uint64_t timestamp_ns = ed.getUnixTimestampNanos();
          const uint32_t video_width = ed.getWidth();
          const uint32_t video_height = ed.getHeight();
          if (data_len == 0) continue;

          bool suppress_video = false;
          bool session_policy = false;
          if (!suppress_video && get_session_policy(control_state.bound_session_id, &session_policy)) {
            suppress_video = session_policy;
          }

          if (suppress_video) {
            suppressed_video_count++;
            if (suppressed_video_count <= 3 || (suppressed_video_count % 500) == 0) {
              printf("[%s] suppress-video drop=%llu session=%s header=%u data=%zu\n",
                     video_service,
                     static_cast<unsigned long long>(suppressed_video_count),
                     control_state.bound_session_id.empty() ? "<legacy>" : control_state.bound_session_id.c_str(),
                     header_len,
                     data_len);
              fflush(stdout);
            }
            continue;
          }

          uint8_t outer_len[4];
          uint8_t video_header[1 + 8 + 4 + 4 + 4];
          const size_t frame_payload_len = sizeof(video_header) + header_len + data_len;
          put_be32(outer_len, static_cast<uint32_t>(frame_payload_len));
          video_header[0] = MSG_VIDEO;
          put_be64(&video_header[1], timestamp_ns);
          put_be32(&video_header[9], video_width);
          put_be32(&video_header[13], video_height);
          put_be32(&video_header[17], header_len);

          std::array<commaview::net::SendBuffer, 4> buffers{{
              {outer_len, sizeof(outer_len)},
              {video_header, sizeof(video_header)},
              {header.begin(), header_len},
              {data.begin(), data_len},
          }};

          // Legacy contract marker: video used to call send_frame_locked(client_fd, payload.data(), payload.size(), &send_mutex).
          // Task 4 keeps the same mutex/status semantics but sends the frame as scatter/gather buffers.
          const auto send_result = send_buffers_locked(client_fd, buffers.data(), buffers.size(), &send_mutex);
          note_video_send_result(send_result);
          if (send_result.status == commaview::net::SendStatus::Backpressure && send_result.bytes_sent == 0) {
            // No bytes from this frame entered the stream; preserve framing and prefer freshness.
            continue;
          }
          if (send_result.status != commaview::net::SendStatus::Ok) {
            note_runtime_peer_disconnect(video_service, "video_send", send_result);
            shutdown(client_fd, SHUT_RDWR);
            goto disconnect;
          }

          frame_count++;
          if (frame_count <= 5 || (frame_count % 200) == 0) {
            auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - t0).count();
            const double fps = elapsed > 0 ? frame_count / elapsed : 0.0;
            printf("[%s] frames=%llu fps=%.1f header=%u data=%zu\n",
                   video_service,
                   static_cast<unsigned long long>(frame_count),
                   fps,
                   header_len,
                   data_len);
            fflush(stdout);
          }
        }
      } catch (const std::exception& e) {
        parse_error_count++;
        printf("[%s] parse exception #%llu: %s (raw=%zu)\n",
               video_service,
               static_cast<unsigned long long>(parse_error_count),
               e.what(),
               raw_size);
        fflush(stdout);
      } catch (...) {
        parse_error_count++;
        printf("[%s] parse unknown exception #%llu (raw=%zu)\n",
               video_service,
               static_cast<unsigned long long>(parse_error_count),
               raw_size);
        fflush(stdout);
      }
    }

    const auto video_loop_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - loop_started);
    note_runtime_loop_sample(false, static_cast<uint64_t>(video_loop_elapsed.count()));

    if (include_telemetry && telemetry_disconnect_requested.load()) {
      break;
    }
  }

disconnect:
  append_runtime_run_event("client_disconnected", video_service, addr_str);
  printf("[%s] client disconnected: %s\n", video_service, addr_str);
  fflush(stdout);

  telemetry_disconnect_requested.store(true);
  shutdown(client_fd, SHUT_RDWR);
  if (telemetry_thread.joinable()) telemetry_thread.join();
  active_counter.fetch_sub(1);
  close(client_fd);

  delete video_poller;
  if (video_sock != nullptr) delete video_sock;
  delete ctx;
}
static void telemetry_loop(int client_fd,
                           const char* stream_name,
                           std::atomic<bool>* disconnect_requested,
                           std::mutex* send_mutex,
                           std::atomic<bool>* telemetry_enabled) {
  if (disconnect_requested == nullptr) return;
  uint64_t telem_raw_count = 0;
  auto next_telem_poll = std::chrono::steady_clock::now();
  auto next_stats_flush = next_telem_poll + std::chrono::milliseconds(1000);
  std::array<uint64_t, static_cast<size_t>(NUM_TELEM)> last_ui_emit_ms = {};
  std::array<uint64_t, static_cast<size_t>(NUM_TELEM)> last_ui_emit_wall_ms = {};

  while (g_running && !disconnect_requested->load()) {
    if (!client_socket_alive(client_fd)) {
      note_runtime_peer_disconnect(stream_name, "telemetry_socket_alive", peer_closed_result());
      disconnect_requested->store(true);
      break;
    }

    if (telemetry_enabled != nullptr && !telemetry_enabled->load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(g_telemetry_emit_ms));
      continue;
    }

    const auto telemetry_started = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now < next_telem_poll) {
      const auto sleep_for_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(next_telem_poll - now).count();
      if (sleep_for_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_for_ms));
      }
      now = std::chrono::steady_clock::now();
    }

    do {
      next_telem_poll += std::chrono::milliseconds(g_telemetry_emit_ms);
    } while (next_telem_poll <= now);

    std::array<bool, static_cast<size_t>(NUM_TELEM)> ui_fresh = {};
    if (g_ui_export_socket != nullptr) {
      for (int i = 0; i < NUM_TELEM; ++i) {
        const char* service_name = kTelemetryServices[static_cast<size_t>(i)];
        const ServicePolicy policy = policy_for_service(g_runtime_state.effective_config, service_name);
        if (!telemetry_policy_fetches_latest(policy)) {
          continue;
        }

        commaview::ui_export::LatestFrame frame;
        if (!g_ui_export_socket->latest_frame(static_cast<uint8_t>(i),
                                              commaview::ui_export::kFreshFrameWindowMs,
                                              &frame)) {
          continue;
        }
        ui_fresh[static_cast<size_t>(i)] = true;
        const uint64_t now_ms = runtime_now_ms();
        if (!telemetry_policy_allows_emit(policy,
                                          frame.updated_at_ms,
                                          now_ms,
                                          last_ui_emit_ms[static_cast<size_t>(i)],
                                          last_ui_emit_wall_ms[static_cast<size_t>(i)])) {
          continue;
        }

        const auto send_started = std::chrono::steady_clock::now();
        auto send_result = send_meta_raw_frame(client_fd,
                                               RAW_META_ENVELOPE_V5,
                                               static_cast<uint8_t>(i),
                                               frame.payload.data(),
                                               frame.payload.size(),
                                               send_mutex);
        const auto send_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - send_started);
        send_result.elapsed_micros = std::max(send_result.elapsed_micros,
                                              static_cast<uint64_t>(send_elapsed.count()));
        const bool sampled = service_policy_samples(policy);
        const bool sent = send_result.status == commaview::net::SendStatus::Ok;
        note_runtime_emit(i,
                          frame.payload.size(),
                          sampled,
                          sent,
                          send_result.elapsed_micros);
        if (!sent) {
          if (commaview::telemetry::telemetry_send_failure_is_droppable(send_result)) {
            append_runtime_run_event("telemetry_drop",
                                     stream_name,
                                     "",
                                     "telemetry_send",
                                     commaview::net::send_status_name(send_result.status));
            last_ui_emit_ms[static_cast<size_t>(i)] = frame.updated_at_ms;
            last_ui_emit_wall_ms[static_cast<size_t>(i)] = now_ms;
            continue;
          }
          note_runtime_peer_disconnect(stream_name, "telemetry_send", send_result);
          disconnect_requested->store(true);
          shutdown(client_fd, SHUT_RDWR);
          break;
        }
        last_ui_emit_ms[static_cast<size_t>(i)] = frame.updated_at_ms;
        last_ui_emit_wall_ms[static_cast<size_t>(i)] = now_ms;
        telem_raw_count++;
      }
    }

    if (disconnect_requested->load()) break;

    if (telem_raw_count > 0 && (telem_raw_count <= 3 || (telem_raw_count % 200) == 0)) {
      printf("[%s] telem_raw=%llu [DIRECT_UI_SOCKET_ONLY] (read+send throttled %dms)\n",
             stream_name,
             static_cast<unsigned long long>(telem_raw_count),
             g_telemetry_emit_ms);
      fflush(stdout);
    }

    now = std::chrono::steady_clock::now();
    if (now >= next_stats_flush) {
      next_stats_flush = now + std::chrono::milliseconds(1000);
      flush_runtime_state();
    }

    const auto telemetry_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - telemetry_started);
    note_runtime_loop_sample(true, static_cast<uint64_t>(telemetry_elapsed.count()));
  }
}

static void handle_telemetry_client(int client_fd) {
  const char* stream_name = "telemetry";
  char addr_str[64] = {};
  struct sockaddr_in peer = {};
  socklen_t peer_len = sizeof(peer);
  if (getpeername(client_fd, reinterpret_cast<struct sockaddr*>(&peer), &peer_len) == 0) {
    inet_ntop(AF_INET, &peer.sin_addr, addr_str, sizeof(addr_str));
  }

  append_runtime_run_event("client_connected", stream_name, addr_str);
  printf("[%s] client connected: %s (fd=%d)\n", stream_name, addr_str, client_fd);
  fflush(stdout);

  int opt = 1;
  setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  int sndbuf = 524288;
  setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  note_runtime_connect();

  std::atomic<bool> disconnect_requested{false};
  std::atomic<bool> telemetry_enabled_for_client{true};
  std::mutex send_mutex;
  std::thread telemetry_thread(telemetry_loop,
                               client_fd,
                               stream_name,
                               &disconnect_requested,
                               &send_mutex,
                               &telemetry_enabled_for_client);
  ClientControlState control_state;

  while (g_running && !disconnect_requested.load()) {
    if (!client_socket_alive(client_fd)) {
      note_runtime_peer_disconnect(stream_name, "client_socket_alive", peer_closed_result());
      break;
    }
    consume_client_control_frames(client_fd, &control_state, stream_name);
    telemetry_enabled_for_client.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  append_runtime_run_event("client_disconnected", stream_name, addr_str);
  printf("[%s] client disconnected: %s\n", stream_name, addr_str);
  fflush(stdout);

  disconnect_requested.store(true);
  shutdown(client_fd, SHUT_RDWR);
  if (telemetry_thread.joinable()) telemetry_thread.join();
  close(client_fd);
}

static void accept_loop(int server_fd, const char* service_name, int port) {
  printf("[%s] listening on :%d\n", service_name, port);
  fflush(stdout);

  while (g_running) {
    struct sockaddr_in client_addr = {};
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(server_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
    if (client_fd < 0) {
      if (g_running) perror("accept");
      continue;
    }
    if (port == PORT_TELEMETRY) {
      std::thread(handle_telemetry_client, client_fd).detach();
    } else {
      std::thread(handle_video_client, client_fd, service_name, port).detach();
    }
  }
}

static void sig_handler(int) { g_running = false; }

int commaview_bridge_main(int argc, char* argv[]) {
  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);
  signal(SIGPIPE, SIG_IGN);



  (void)argc;
  (void)argv;
  initialize_runtime_state_once();
  append_runtime_run_event("process_start");

  g_ui_export_socket = std::make_unique<commaview::ui_export::SocketServer>();
  const bool ui_export_socket_ready = g_ui_export_socket->start();

  const char** video_services = VIDEO_SERVICES_PROD;
  printf("CommaView Bridge v3.3.8-safe-bundle (C++) [VIDEO+TELEMETRY][RAW_ONLY_DEFAULT][DIRECT_V2_UI_EXPORT_DEFAULT][UI_SOCKET_PREFERRED=%s][META_MODE=raw-only][EMIT_MS=%d]\n",
         ui_export_socket_ready ? "on" : "off",
         g_telemetry_emit_ms);
  fflush(stdout);

  std::vector<std::pair<int, const char*>> streams;
  streams.push_back({PORT_ROAD, video_services[0]});
  streams.push_back({PORT_WIDE, video_services[1]});
  streams.push_back({PORT_DRIVER, video_services[2]});
  streams.push_back({PORT_TELEMETRY, "telemetry"});

  std::vector<std::thread> threads;
  std::vector<int> server_fds;

  for (auto& s : streams) {
    int fd = create_server(s.first);
    if (fd < 0) {
      fprintf(stderr, "failed on port %d\n", s.first);
      return 1;
    }
    server_fds.push_back(fd);
    threads.emplace_back(accept_loop, fd, s.second, s.first);
  }

  append_runtime_run_event("ready");
  printf("ready. waiting for connections...\n");
  fflush(stdout);

  for (auto& t : threads) t.join();
  for (int fd : server_fds) close(fd);
  if (g_ui_export_socket != nullptr) g_ui_export_socket->stop();

  append_runtime_run_event("process_stop");
  printf("CommaView Bridge stopped.\n");
  return 0;
}
#ifndef COMMAVIEW_BRIDGE_NO_MAIN
int main(int argc, char* argv[]) {
  return commaview_bridge_main(argc, argv);
}
#endif
