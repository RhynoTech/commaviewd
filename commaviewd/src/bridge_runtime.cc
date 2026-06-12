#include "framing.h"
#include "socket.h"
#include "policy.h"
#include "router.h"
#include "telemetry_policy.h"
#include "runtime_debug_config.h"
#include "runtime_video_send_accounting.h"
#include "ui_export_socket.h"
#include "udp_video_sender.h"
#include "video_transport_policy.h"
/**
 * CommaView Unified Bridge (C++)
 *
 * Streams HEVC video over UDP and raw telemetry over TCP.
 *
 * Ports (one per stream):
 *   8200 road, 8201 wide, 8202 driver  — UDP video (CVUP datagrams) + TCP legacy
 *                                        control/telemetry companion channel
 *   8203 telemetry                     — TCP telemetry stream
 *
 * UDP video lifecycle (see docs/udp-video-protocol.md):
 *   The app sends Hello/Heartbeat/Policy datagrams to the stream port; the
 *   runtime starts sending packetized frames while a client checked in within
 *   the liveness window, and serves RepairRequest resends from a short cache.
 *
 * TCP framing (telemetry + control):
 *   [4-byte big-endian length][payload]
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
#include <fcntl.h>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
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
#include <utility>

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


static constexpr uint8_t MSG_CONTROL = 0x03;
static constexpr uint8_t MSG_META_RAW = 0x04;
static constexpr uint8_t RAW_META_ENVELOPE_V4 = 0x04;
static constexpr uint8_t RAW_META_ENVELOPE_V5 = 0x05;

static constexpr int PORT_ROAD = 8200;
static constexpr int PORT_WIDE = 8201;
static constexpr int PORT_DRIVER = 8202;
static constexpr int PORT_TELEMETRY = 8203;
static constexpr uint64_t VIDEO_SEND_BUDGET_MICROS = 16000ULL;
static constexpr size_t VIDEO_FRAME_QUEUE_CAPACITY = 3;

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

static commaview::video::UdpVideoStreamId udp_stream_id_for_port(int port) {
  if (port == PORT_WIDE) return commaview::video::UdpVideoStreamId::Wide;
  if (port == PORT_DRIVER) return commaview::video::UdpVideoStreamId::Driver;
  return commaview::video::UdpVideoStreamId::Road;
}






static size_t queue_size_for_service(const char* service_name) {
  auto it = services.find(std::string(service_name));
  if (it == services.end()) return 0;
  return it->second.queue_size;
}

static void put_be32(uint8_t* buf, uint32_t val) {
  commaview::net::put_be32(buf, val);
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
  commaview::runtime::RuntimeVideoSendStats video_send = {};
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

static int64_t runtime_now_ns() {
  return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
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
  out << "\"videoSend\":" << commaview::runtime::video_send_stats_json(g_runtime_state.video_send) << ",";
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

static void note_video_queue_deltas(uint64_t queue_drop_delta,
                                    uint64_t keyframe_wait_drop_delta,
                                    size_t queue_high_watermark) {
  if (queue_drop_delta == 0 && keyframe_wait_drop_delta == 0 && queue_high_watermark == 0) return;
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  g_runtime_state.video_send.queue_drop_count += queue_drop_delta;
  g_runtime_state.video_send.keyframe_wait_drop_count += keyframe_wait_drop_delta;
  g_runtime_state.video_send.queue_high_watermark = std::max<uint64_t>(
      g_runtime_state.video_send.queue_high_watermark,
      static_cast<uint64_t>(queue_high_watermark));
}

static void note_video_queued_frame_age(uint64_t age_ms) {
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  g_runtime_state.video_send.max_queued_frame_age_ms = std::max(
      g_runtime_state.video_send.max_queued_frame_age_ms,
      age_ms);
}

static void note_udp_video_send_stats(const commaview::video::UdpVideoSendStats& stats) {
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  g_runtime_state.video_send.udp_packets_sent += stats.packets_sent;
  g_runtime_state.video_send.udp_send_error_count += stats.send_errors;
  g_runtime_state.video_send.udp_no_client_drop_count += stats.dropped_no_client;
  g_runtime_state.video_send.udp_suppressed_drop_count += stats.dropped_suppressed;
  g_runtime_state.video_send.udp_repair_cache_bytes = stats.repair_cache_bytes;
  g_runtime_state.video_send.udp_repair_cache_high_watermark_bytes = std::max<uint64_t>(
      g_runtime_state.video_send.udp_repair_cache_high_watermark_bytes,
      stats.repair_cache_high_watermark_bytes);
}

static void note_udp_video_repair_stats(const commaview::video::UdpVideoRepairStats& stats) {
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  g_runtime_state.video_send.udp_repair_requests += stats.requests;
  g_runtime_state.video_send.udp_repair_packets_resent += stats.packets_resent;
  g_runtime_state.video_send.udp_repair_miss_count += stats.missing_count;
  g_runtime_state.video_send.udp_send_error_count += stats.send_errors;
  g_runtime_state.video_send.udp_repair_cache_bytes = stats.repair_cache_bytes;
  g_runtime_state.video_send.udp_repair_cache_high_watermark_bytes = std::max<uint64_t>(
      g_runtime_state.video_send.udp_repair_cache_high_watermark_bytes,
      stats.repair_cache_high_watermark_bytes);
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

using ClientControlState = commaview::control::ClientControlState;

static void consume_client_control_frames(int client_fd,
                                          ClientControlState* state,
                                          const char* video_service) {
  commaview::control::consume_client_control_frames(client_fd, state, video_service, MSG_CONTROL);
}

static bool client_socket_alive(int fd) {
  return commaview::net::client_socket_alive(fd);
}

static int create_server(int port) {
  return commaview::net::create_server(port);
}

static uint16_t read_u16_be(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

static uint32_t read_u32_be(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

static int create_udp_video_socket(int port) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    perror("udp video socket");
    return -1;
  }

  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  // Keyframes burst dozens of datagrams at once; a large send buffer keeps
  // those bursts in the kernel instead of dropping them at the socket.
  int sndbuf = 4 * 1024 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  int rcvbuf = 1024 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    perror("udp video bind");
    close(fd);
    return -1;
  }

  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  return fd;
}

static constexpr int UDP_VIDEO_SEND_RETRY_MS = 5;

// Non-blocking send with a short bounded wait when the socket buffer is full,
// so transient bursts degrade to brief pacing instead of dropped packets.
static ssize_t send_udp_video_datagram(int udp_fd,
                                       const uint8_t* data,
                                       size_t size,
                                       const sockaddr_storage& addr,
                                       socklen_t addr_len) {
  if (udp_fd < 0) return -1;
  for (int attempt = 0; attempt < 3; ++attempt) {
    const ssize_t sent = sendto(udp_fd,
                                data,
                                size,
                                MSG_DONTWAIT,
                                reinterpret_cast<const sockaddr*>(&addr),
                                addr_len);
    if (sent >= 0) return sent;
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    pollfd pfd{};
    pfd.fd = udp_fd;
    pfd.events = POLLOUT;
    if (poll(&pfd, 1, UDP_VIDEO_SEND_RETRY_MS) <= 0) return -1;
  }
  return -1;
}

struct UdpVideoControlDatagram {
  commaview::video::UdpVideoPacketType type = commaview::video::UdpVideoPacketType::Heartbeat;
  commaview::video::UdpVideoStreamId stream_id = commaview::video::UdpVideoStreamId::Road;
  uint16_t session_id = 0;
  uint32_t frame_sequence = 0;
  bool has_suppress_video = false;
  bool suppress_video = false;
  std::vector<uint16_t> packet_indexes;
};

static bool parse_udp_video_control_datagram(const uint8_t* data,
                                             size_t size,
                                             commaview::video::UdpVideoStreamId expected_stream,
                                             UdpVideoControlDatagram* out) {
  if (data == nullptr || out == nullptr || size < 10) return false;
  if (read_u32_be(data) != commaview::video::UDP_VIDEO_MAGIC) return false;
  if (data[4] != commaview::video::UDP_VIDEO_VERSION) return false;

  const auto type = static_cast<commaview::video::UdpVideoPacketType>(data[5]);
  if (type != commaview::video::UdpVideoPacketType::Hello &&
      type != commaview::video::UdpVideoPacketType::Heartbeat &&
      type != commaview::video::UdpVideoPacketType::Policy &&
      type != commaview::video::UdpVideoPacketType::RepairRequest) {
    return false;
  }

  const auto stream = static_cast<commaview::video::UdpVideoStreamId>(data[6]);
  if (stream != expected_stream) return false;
  if (data[7] != 0) return false;

  UdpVideoControlDatagram parsed;
  parsed.type = type;
  parsed.stream_id = stream;
  parsed.session_id = read_u16_be(data + 8);

  if ((type == commaview::video::UdpVideoPacketType::Heartbeat ||
       type == commaview::video::UdpVideoPacketType::Policy) &&
      size >= 11) {
    parsed.has_suppress_video = true;
    parsed.suppress_video = data[10] != 0;
  }

  if (type == commaview::video::UdpVideoPacketType::RepairRequest) {
    if (size < 60) return false;
    parsed.frame_sequence = read_u32_be(data + 28);
    const uint32_t payload_len = read_u32_be(data + 56);
    if (payload_len > size - 60 || (payload_len % 2) != 0) return false;
    for (size_t pos = 60; pos < 60 + payload_len; pos += 2) {
      parsed.packet_indexes.push_back(read_u16_be(data + pos));
    }
  }

  *out = std::move(parsed);
  return true;
}

static void drain_udp_video_control_datagrams(int udp_fd,
                                              commaview::video::UdpVideoStreamId expected_stream,
                                              commaview::video::UdpVideoSender* sender) {
  if (udp_fd < 0 || sender == nullptr) return;

  std::array<uint8_t, commaview::video::UDP_VIDEO_MAX_DATAGRAM_BYTES> buffer{};
  while (true) {
    sockaddr_storage peer{};
    socklen_t peer_len = sizeof(peer);
    const ssize_t n = recvfrom(udp_fd,
                               buffer.data(),
                               buffer.size(),
                               MSG_DONTWAIT,
                               reinterpret_cast<sockaddr*>(&peer),
                               &peer_len);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) perror("udp video recvfrom");
      break;
    }
    if (n == 0) continue;

    UdpVideoControlDatagram datagram;
    if (!parse_udp_video_control_datagram(buffer.data(), static_cast<size_t>(n), expected_stream, &datagram)) {
      continue;
    }

    if (datagram.type == commaview::video::UdpVideoPacketType::RepairRequest) {
      commaview::video::UdpVideoRepairRequest request;
      request.stream_id = datagram.stream_id;
      request.session_id = datagram.session_id;
      request.frame_sequence = datagram.frame_sequence;
      request.packet_indexes = std::move(datagram.packet_indexes);
      const auto stats = sender->handle_repair_request(request, runtime_now_ns());
      note_udp_video_repair_stats(stats);
      continue;
    }

    if (datagram.has_suppress_video) {
      sender->note_client_policy(datagram.stream_id,
                                 peer,
                                 peer_len,
                                 datagram.session_id,
                                 datagram.suppress_video,
                                 runtime_now_ns());
      continue;
    }

    sender->note_client_hello(datagram.stream_id, peer, peer_len, datagram.session_id, runtime_now_ns());
  }
}

static cereal::EncodeData::Reader read_encode_data(cereal::Event::Reader event, int port) {
  return commaview::video::read_encode_data(event, port, false);
}

static bool wait_for_udp_video_datagram(int udp_fd, int timeout_ms) {
  pollfd pfd{};
  pfd.fd = udp_fd;
  pfd.events = POLLIN;
  return poll(&pfd, 1, timeout_ms) > 0;
}

static constexpr int UDP_VIDEO_IDLE_POLL_MS = 200;
static constexpr int UDP_VIDEO_ACTIVE_POLL_MS = 20;

// Per-stream UDP video pump. Runs for the lifetime of the runtime and is fully
// independent of TCP clients: a client announces itself with Hello/Heartbeat
// datagrams, video flows while the client stays within the liveness window,
// and the encoder subscription is dropped while no client is active.
static void udp_video_stream_loop(int port, const char* video_service) {
  const auto udp_stream_id = udp_stream_id_for_port(port);
  const int udp_fd = create_udp_video_socket(port);
  if (udp_fd < 0) {
    fprintf(stderr, "[%s] failed to open UDP video socket on :%d\n", video_service, port);
    return;
  }
  printf("[%s] UDP video on :%d\n", video_service, port);
  fflush(stdout);

  auto udp_send_fn = [udp_fd](const uint8_t* data,
                              size_t size,
                              const sockaddr_storage& addr,
                              socklen_t addr_len) -> ssize_t {
    return send_udp_video_datagram(udp_fd, data, size, addr, addr_len);
  };
  commaview::video::UdpVideoSender udp_video_sender(udp_send_fn);

  Context* ctx = Context::create();
  SubSocket* video_sock = nullptr;
  Poller* video_poller = nullptr;
  bool client_was_active = false;

  uint64_t frame_count = 0;
  uint64_t parsed_frame_count = 0;
  uint64_t parse_error_count = 0;
  uint64_t wrong_union_count = 0;
  uint64_t suppressed_video_count = 0;
  commaview::video::VideoFrameQueue video_queue(VIDEO_FRAME_QUEUE_CAPACITY);
  uint64_t last_queue_drop_count = 0;
  uint64_t last_keyframe_wait_drop_count = 0;
  auto t0 = std::chrono::steady_clock::now();
  AlignedBuffer aligned_buf;

  while (g_running) {
    const auto loop_started = std::chrono::steady_clock::now();
    drain_udp_video_control_datagrams(udp_fd, udp_stream_id, &udp_video_sender);

    const bool client_active = udp_video_sender.has_active_client(udp_stream_id, runtime_now_ns());
    if (!client_active) {
      if (client_was_active) {
        client_was_active = false;
        append_runtime_run_event("udp_client_idle", video_service);
        printf("[%s] UDP client idle, pausing video\n", video_service);
        fflush(stdout);
        // Drop queued frames so a returning client never receives stale video.
        while (video_queue.pop_next()) {
        }
      }
      if (video_sock != nullptr) {
        delete video_poller;
        video_poller = nullptr;
        delete video_sock;
        video_sock = nullptr;
      }
      wait_for_udp_video_datagram(udp_fd, UDP_VIDEO_IDLE_POLL_MS);
      continue;
    }

    if (!client_was_active) {
      client_was_active = true;
      append_runtime_run_event("udp_client_active", video_service);
      printf("[%s] UDP client active, streaming video\n", video_service);
      fflush(stdout);
      note_runtime_connect();
    }
    if (video_sock == nullptr) {
      const size_t video_segment_size = queue_size_for_service(video_service);
      video_sock = SubSocket::create(ctx, video_service, "127.0.0.1", true, true, video_segment_size);
      video_poller = Poller::create();
      if (video_sock != nullptr) video_poller->registerSocket(video_sock);
    }

    auto ready = video_poller->poll(UDP_VIDEO_ACTIVE_POLL_MS);

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

          if (udp_video_sender.client_suppresses_video(udp_stream_id)) {
            suppressed_video_count++;
            if (suppressed_video_count <= 3 || (suppressed_video_count % 500) == 0) {
              printf("[%s] suppress-video drop=%llu header=%u data=%zu\n",
                     video_service,
                     static_cast<unsigned long long>(suppressed_video_count),
                     header_len,
                     data_len);
              fflush(stdout);
            }
            continue;
          }

          const bool is_keyframe =
              commaview::video::contains_hevc_idr(header.begin(), header_len) ||
              commaview::video::contains_hevc_idr(data.begin(), data_len);
          commaview::video::PendingVideoFrame pending;
          pending.sequence = ++parsed_frame_count;
          pending.is_keyframe = is_keyframe;
          pending.created_at_ms = runtime_now_ms();
          pending.timestamp_ns = timestamp_ns;
          pending.width = video_width;
          pending.height = video_height;
          pending.codec_header.assign(header.begin(), header.begin() + header_len);
          pending.data.assign(data.begin(), data.begin() + data_len);
          video_queue.push(std::move(pending));
          note_video_queue_deltas(
              video_queue.drop_count() - last_queue_drop_count,
              video_queue.keyframe_wait_drop_count() - last_keyframe_wait_drop_count,
              video_queue.high_watermark());
          last_queue_drop_count = video_queue.drop_count();
          last_keyframe_wait_drop_count = video_queue.keyframe_wait_drop_count();

          while (auto queued = video_queue.pop_next()) {
            note_video_queue_deltas(
                video_queue.drop_count() - last_queue_drop_count,
                video_queue.keyframe_wait_drop_count() - last_keyframe_wait_drop_count,
                video_queue.high_watermark());
            last_queue_drop_count = video_queue.drop_count();
            last_keyframe_wait_drop_count = video_queue.keyframe_wait_drop_count();
            note_video_queued_frame_age(runtime_now_ms() - queued->created_at_ms);
            const uint32_t queued_header_len = static_cast<uint32_t>(queued->codec_header.size());
            commaview::video::UdpVideoFrameForPacketizing frame;
            frame.stream_id = udp_stream_id;
            frame.frame_sequence = static_cast<uint32_t>(queued->sequence);
            frame.timestamp_nanos = queued->timestamp_ns;
            frame.width = queued->width;
            frame.height = queued->height;
            frame.is_keyframe = queued->is_keyframe;
            frame.codec_header = queued->codec_header;
            frame.data = queued->data;

            const auto udp_stats = udp_video_sender.send_frame(frame, runtime_now_ns());
            note_udp_video_send_stats(udp_stats);
            if (udp_stats.send_errors > 0 &&
                (udp_stats.send_errors <= 5 || (udp_stats.send_errors % 200) == 0)) {
              printf("[%s] udp video send errors=%llu packets=%llu sent=%llu\n",
                     video_service,
                     static_cast<unsigned long long>(udp_stats.send_errors),
                     static_cast<unsigned long long>(udp_stats.packets_packetized),
                     static_cast<unsigned long long>(udp_stats.packets_sent));
              fflush(stdout);
            }

            frame_count++;
            if (frame_count <= 5 || (frame_count % 200) == 0) {
              auto now = std::chrono::steady_clock::now();
              const double elapsed = std::chrono::duration<double>(now - t0).count();
              const double fps = elapsed > 0 ? frame_count / elapsed : 0.0;
              printf("[%s] frames=%llu fps=%.1f header=%u data=%zu key=%d\n",
                     video_service,
                     static_cast<unsigned long long>(frame_count),
                     fps,
                     queued_header_len,
                     queued->data.size(),
                     queued->is_keyframe ? 1 : 0);
              fflush(stdout);
            }
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
  }

  close(udp_fd);
  delete video_poller;
  if (video_sock != nullptr) delete video_sock;
  delete ctx;
}

// Legacy TCP companion channel on the video ports. Video itself is UDP-only;
// this channel remains for control frames and telemetry-on-video clients.
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

  const bool include_telemetry = (port == PORT_ROAD || port == PORT_WIDE);

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

  ClientControlState control_state;

  while (g_running) {
    if (!client_socket_alive(client_fd)) {
      note_runtime_peer_disconnect(video_service, "client_socket_alive", peer_closed_result());
      break;
    }

    consume_client_control_frames(client_fd, &control_state, video_service);
    if (include_telemetry) {
      telemetry_enabled_for_client.store(control_state.telemetry_on_video);
      if (telemetry_disconnect_requested.load()) break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  append_runtime_run_event("client_disconnected", video_service, addr_str);
  printf("[%s] client disconnected: %s\n", video_service, addr_str);
  fflush(stdout);

  telemetry_disconnect_requested.store(true);
  shutdown(client_fd, SHUT_RDWR);
  if (telemetry_thread.joinable()) telemetry_thread.join();
  active_counter.fetch_sub(1);
  close(client_fd);
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

  // UDP video pumps run independently of any TCP client.
  threads.emplace_back(udp_video_stream_loop, PORT_ROAD, video_services[0]);
  threads.emplace_back(udp_video_stream_loop, PORT_WIDE, video_services[1]);
  threads.emplace_back(udp_video_stream_loop, PORT_DRIVER, video_services[2]);

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
