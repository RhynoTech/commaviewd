#include "framing.h"
#include "socket.h"
#include "policy.h"
#include "router.h"
#include "telemetry_policy.h"
#include "runtime_debug_config.h"
/**
 * CommaView Unified Bridge (C++)
 *
 * Streams HEVC video and raw telemetry on road+wide ports.
 *
 * Ports:
 *   8200 road, 8201 wide, 8202 driver
 *
 * Framing:
 *   [4-byte big-endian length][payload]
 *   payload[0] = 0x01 (video): [type][header_len_be32][header][data]
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


static constexpr uint8_t MSG_VIDEO = 0x01;
static constexpr uint8_t MSG_CONTROL = 0x03;
static constexpr uint8_t MSG_META_RAW = 0x04;

static constexpr int PORT_ROAD = 8200;
static constexpr int PORT_WIDE = 8201;
static constexpr int PORT_DRIVER = 8202;

// Runtime policy defaults favor only the services we currently trust on-road.
static const char* TELEMETRY_SERVICES[] = {
  "carState", "selfdriveState", "deviceState", "liveCalibration", "radarState", "modelV2",
  "carControl", "carOutput", "liveParameters", "driverMonitoringState",
  "driverStateV2", "onroadEvents", "roadCameraState",
  "alertDebug", "modelDataV2SP", "longitudinalPlanSP"
};
static constexpr int NUM_TELEM = 16;
static constexpr int TELEMETRY_EMIT_MS_DEFAULT = 50;  // 20 Hz base poll for PASS and future SAMPLE modes
static int g_telemetry_emit_ms = TELEMETRY_EMIT_MS_DEFAULT;

static const char* VIDEO_SERVICES_PROD[] = {
  "roadEncodeData", "wideRoadEncodeData", "driverEncodeData"
};


static std::atomic<bool> g_running{true};


static std::atomic<int> g_active_road{0};
static std::atomic<int> g_active_wide{0};
static std::atomic<int> g_active_driver{0};

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


static void put_be16(uint8_t* buf, uint16_t val) {
  buf[0] = (val >> 8) & 0xFF;
  buf[1] = val & 0xFF;
}

static void put_be64(uint8_t* buf, uint64_t val) {
  buf[0] = (val >> 56) & 0xFF;
  buf[1] = (val >> 48) & 0xFF;
  buf[2] = (val >> 40) & 0xFF;
  buf[3] = (val >> 32) & 0xFF;
  buf[4] = (val >> 24) & 0xFF;
  buf[5] = (val >> 16) & 0xFF;
  buf[6] = (val >> 8) & 0xFF;
  buf[7] = val & 0xFF;
}

static int telemetry_index_for_which(cereal::Event::Which which) {
  switch (which) {
    case cereal::Event::CAR_STATE: return 0;
    case cereal::Event::SELFDRIVE_STATE: return 1;
    case cereal::Event::DEVICE_STATE: return 2;
    case cereal::Event::LIVE_CALIBRATION: return 3;
    case cereal::Event::RADAR_STATE: return 4;
    case cereal::Event::MODEL_V2: return 5;
    case cereal::Event::CAR_CONTROL: return 6;
    case cereal::Event::CAR_OUTPUT: return 7;
    case cereal::Event::LIVE_PARAMETERS: return 8;
    case cereal::Event::DRIVER_MONITORING_STATE: return 9;
    case cereal::Event::DRIVER_STATE_V2: return 10;
    case cereal::Event::ONROAD_EVENTS: return 11;
    case cereal::Event::ROAD_CAMERA_STATE: return 12;
    default: return -1;
  }
}


static bool send_meta_raw_frame(int fd,
                                uint8_t service_index,
                                const uint8_t* raw_data,
                                size_t raw_size) {
  if (raw_data == nullptr || raw_size == 0) return true;
  const uint32_t raw_len = static_cast<uint32_t>(raw_size);
  std::vector<uint8_t> payload(1 + 1 + 4 + raw_len);
  payload[0] = 0x04;  // raw meta envelope v4 is service index + raw event only
  payload[1] = service_index;
  put_be32(&payload[2], raw_len);
  memcpy(&payload[6], raw_data, raw_len);
  return commaview::net::send_meta_bytes(fd, payload.data(), payload.size(), MSG_META_RAW);
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
};

struct RuntimeLoopStats {
  uint64_t iterations = 0;
  uint64_t total_micros = 0;
  uint64_t max_micros = 0;
  uint64_t over_budget = 0;
};

struct RuntimeState {
  LoadedRuntimeDebugConfig loaded_config = {};
  LoadedRuntimeDebugConfig effective_config = {};
  std::vector<RuntimeServiceStats> services = std::vector<RuntimeServiceStats>(NUM_TELEM);
  RuntimeLoopStats telemetry_loop = {};
  RuntimeLoopStats video_loop = {};
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
  out << "\"services\":{";
  for (int i = 0; i < NUM_TELEM; ++i) {
    if (i > 0) out << ",";
    const char* service_name = TELEMETRY_SERVICES[i];
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
    out << "\"maxSendStallMicros\":" << stats.max_send_stall_micros;
    out << "}";
  }
  out << "}}";
  return out.str();
}

static void flush_runtime_state_locked() {
  write_text_file_best_effort(runtime_debug_effective_path(), render_config_json(g_runtime_state.effective_config, true));
  write_text_file_best_effort(runtime_debug_stats_path(), build_runtime_stats_json_locked());
}

static void initialize_runtime_state_once() {
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
  flush_runtime_state_locked();
}

static ServicePolicy runtime_policy_for_index(int idx) {
  if (idx < 0 || idx >= NUM_TELEM) return {};
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  if (!g_runtime_state.initialized) return default_service_policy_for_name(TELEMETRY_SERVICES[idx]);
  return policy_for_service(g_runtime_state.effective_config, TELEMETRY_SERVICES[idx]);
}

static void note_runtime_subscriber_delta(int idx, int delta) {
  if (idx < 0 || idx >= NUM_TELEM) return;
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  RuntimeServiceStats& stats = g_runtime_state.services[static_cast<size_t>(idx)];
  if (delta >= 0) {
    stats.active_subscribers += static_cast<uint64_t>(delta);
  } else if (stats.active_subscribers >= static_cast<uint64_t>(-delta)) {
    stats.active_subscribers -= static_cast<uint64_t>(-delta);
  } else {
    stats.active_subscribers = 0;
  }
  flush_runtime_state_locked();
}

static void note_runtime_connect() {
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  g_runtime_state.reconnect_count += 1;
  flush_runtime_state_locked();
}

static void note_runtime_receive(int idx, size_t bytes) {
  if (idx < 0 || idx >= NUM_TELEM) return;
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  RuntimeServiceStats& stats = g_runtime_state.services[static_cast<size_t>(idx)];
  stats.receive_count += 1;
  stats.receive_bytes += static_cast<uint64_t>(bytes);
  stats.last_receive_ms = runtime_now_ms();
}

static void note_runtime_drain(int idx, uint64_t discarded, uint64_t burst) {
  if (idx < 0 || idx >= NUM_TELEM) return;
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  RuntimeServiceStats& stats = g_runtime_state.services[static_cast<size_t>(idx)];
  stats.drain_count += discarded;
  stats.max_drained_burst = std::max(stats.max_drained_burst, burst);
}

static void note_runtime_emit(int idx, size_t bytes, bool sampled, bool ok, uint64_t stall_micros) {
  if (idx < 0 || idx >= NUM_TELEM) return;
  std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
  RuntimeServiceStats& stats = g_runtime_state.services[static_cast<size_t>(idx)];
  stats.max_send_stall_micros = std::max(stats.max_send_stall_micros, stall_micros);
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

static bool send_all(int fd, const void* data, size_t len) {
  return commaview::net::send_all(fd, data, len);
}

static bool send_frame(int fd, const uint8_t* payload, size_t payload_len) {
  return commaview::net::send_frame(fd, payload, payload_len);
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

static void handle_client(int client_fd, const char* video_service, int port) {
  char addr_str[64] = {};
  struct sockaddr_in peer = {};
  socklen_t peer_len = sizeof(peer);
  if (getpeername(client_fd, reinterpret_cast<struct sockaddr*>(&peer), &peer_len) == 0) {
    inet_ntop(AF_INET, &peer.sin_addr, addr_str, sizeof(addr_str));
  }

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
  SubSocket* telem_socks[NUM_TELEM] = {};
  ServicePolicy telem_policies[NUM_TELEM] = {};

  if (include_telemetry) {
    for (int i = 0; i < NUM_TELEM; i++) {
      telem_policies[i] = runtime_policy_for_index(i);
      if (!service_policy_subscribes(telem_policies[i])) continue;
      const size_t segment_size = queue_size_for_service(TELEMETRY_SERVICES[i]);
      const bool conflate = !service_policy_samples(telem_policies[i]);
      telem_socks[i] = SubSocket::create(ctx, TELEMETRY_SERVICES[i], "127.0.0.1", conflate, true, segment_size);
      if (telem_socks[i] != nullptr) note_runtime_subscriber_delta(i, 1);
    }
  }

  Poller* video_poller = Poller::create();
  if (video_sock != nullptr) video_poller->registerSocket(video_sock);

  Poller* telemetry_poller = nullptr;
  if (include_telemetry) {
    telemetry_poller = Poller::create();
    for (int i = 0; i < NUM_TELEM; i++) {
      if (telem_socks[i] != nullptr) telemetry_poller->registerSocket(telem_socks[i]);
    }
  }

  uint64_t frame_count = 0;
  uint64_t parse_error_count = 0;
  uint64_t wrong_union_count = 0;
  uint64_t telem_raw_count = 0;
  uint64_t telem_drain_count = 0;
  uint64_t suppressed_video_count = 0;
  auto t0 = std::chrono::steady_clock::now();
  auto next_telem_poll = t0;
  auto next_stats_flush = t0 + std::chrono::milliseconds(1000);
  std::vector<std::vector<uint8_t>> sampled_latest(NUM_TELEM);
  std::vector<bool> sampled_have_latest(NUM_TELEM, false);
  std::vector<std::chrono::steady_clock::time_point> sampled_next_emit(NUM_TELEM, t0);
  AlignedBuffer aligned_buf;
  ClientControlState control_state;

  while (g_running) {
    const auto loop_started = std::chrono::steady_clock::now();
    if (!client_socket_alive(client_fd)) break;

    consume_client_control_frames(client_fd, &control_state, video_service);

    int video_poll_ms = 20;
    if (include_telemetry) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= next_telem_poll) {
        video_poll_ms = 0;
      } else {
        const auto millis_until_telem =
            std::chrono::duration_cast<std::chrono::milliseconds>(next_telem_poll - now).count();
        if (millis_until_telem < video_poll_ms) {
          video_poll_ms = static_cast<int>(millis_until_telem);
        }
      }
    }

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

          const size_t payload_len = 1 + 4 + header_len + data_len;
          std::vector<uint8_t> payload(payload_len);
          payload[0] = MSG_VIDEO;
          put_be32(&payload[1], header_len);
          if (header_len > 0) memcpy(&payload[5], header.begin(), header_len);
          memcpy(&payload[5 + header_len], data.begin(), data_len);

          if (!send_frame(client_fd, payload.data(), payload.size())) goto disconnect;

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

    if (include_telemetry && telemetry_poller != nullptr) {
      const auto telemetry_started = std::chrono::steady_clock::now();
      auto now = std::chrono::steady_clock::now();
      if (now >= next_telem_poll) {
        do {
          next_telem_poll += std::chrono::milliseconds(g_telemetry_emit_ms);
        } while (next_telem_poll <= now);

        auto telem_ready = telemetry_poller->poll(0);
        for (auto* sock : telem_ready) {
          int telem_sock_idx = -1;
          for (int i = 0; i < NUM_TELEM; i++) {
            if (sock == telem_socks[i]) {
              telem_sock_idx = i;
              break;
            }
          }
          if (telem_sock_idx < 0) continue;

          if (service_policy_samples(telem_policies[telem_sock_idx])) {
            uint64_t drained_messages = 0;
            while (true) {
              std::unique_ptr<Message> drained(sock->receive(true));
              if (!drained) break;
              drained_messages++;
              const size_t drained_size = drained->getSize();
              const uint8_t* drained_ptr = reinterpret_cast<const uint8_t*>(drained->getData());
              note_runtime_receive(telem_sock_idx, drained_size);
              sampled_latest[telem_sock_idx].assign(drained_ptr, drained_ptr + drained_size);
              sampled_have_latest[telem_sock_idx] = true;
            }
            if (drained_messages > 1) {
              const uint64_t discarded = drained_messages - 1;
              telem_drain_count += discarded;
              note_runtime_drain(telem_sock_idx, discarded, discarded);
            }
            continue;
          }

          std::unique_ptr<Message> raw_msg(sock->receive(true));
          if (!raw_msg) continue;
          const size_t raw_size = raw_msg->getSize();
          const uint8_t* raw_ptr = reinterpret_cast<const uint8_t*>(raw_msg->getData());
          note_runtime_receive(telem_sock_idx, raw_size);
          const auto send_started = std::chrono::steady_clock::now();
          const bool sent = send_meta_raw_frame(client_fd,
                                                static_cast<uint8_t>(telem_sock_idx),
                                                raw_ptr,
                                                raw_size);
          const auto send_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - send_started);
          note_runtime_emit(telem_sock_idx,
                            raw_size,
                            false,
                            sent,
                            static_cast<uint64_t>(send_elapsed.count()));
          if (!sent) goto disconnect;
          telem_raw_count++;
        }

        now = std::chrono::steady_clock::now();
        for (int i = 0; i < NUM_TELEM; ++i) {
          if (!service_policy_samples(telem_policies[i])) continue;
          if (!sampled_have_latest[i]) continue;
          if (now < sampled_next_emit[i]) continue;
          const auto& latest = sampled_latest[static_cast<size_t>(i)];
          if (latest.empty()) continue;
          const auto send_started = std::chrono::steady_clock::now();
          const bool sent = send_meta_raw_frame(client_fd,
                                                static_cast<uint8_t>(i),
                                                latest.data(),
                                                latest.size());
          const auto send_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - send_started);
          note_runtime_emit(i,
                            latest.size(),
                            true,
                            sent,
                            static_cast<uint64_t>(send_elapsed.count()));
          if (!sent) goto disconnect;
          telem_raw_count++;
          const int sample_hz = std::max(telem_policies[i].sample_hz, 1);
          const auto sample_interval = std::chrono::milliseconds(std::max(1, 1000 / sample_hz));
          do {
            sampled_next_emit[i] += sample_interval;
          } while (sampled_next_emit[i] <= now);
        }

        if (telem_raw_count > 0 && (telem_raw_count <= 3 || (telem_raw_count % 200) == 0)) {
          printf("[%s] telem_raw=%llu drain=%llu (read+send throttled %dms)\n",
                 video_service,
                 static_cast<unsigned long long>(telem_raw_count),
                 static_cast<unsigned long long>(telem_drain_count),
                 g_telemetry_emit_ms);
          fflush(stdout);
        }
      }

      now = std::chrono::steady_clock::now();
      if (now >= next_stats_flush) {
        next_stats_flush = now + std::chrono::milliseconds(1000);
        std::lock_guard<std::mutex> lock(g_runtime_state_mutex);
        flush_runtime_state_locked();
      }

      const auto telemetry_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - telemetry_started);
      note_runtime_loop_sample(true, static_cast<uint64_t>(telemetry_elapsed.count()));
    }
  }

disconnect:
  printf("[%s] client disconnected: %s\n", video_service, addr_str);
  fflush(stdout);

  active_counter.fetch_sub(1);
  close(client_fd);

  delete video_poller;
  if (telemetry_poller != nullptr) delete telemetry_poller;
  if (video_sock != nullptr) delete video_sock;
  for (int i = 0; i < NUM_TELEM; i++) {
    if (telem_socks[i] != nullptr) {
      note_runtime_subscriber_delta(i, -1);
      delete telem_socks[i];
    }
  }
  delete ctx;
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
    std::thread(handle_client, client_fd, service_name, port).detach();
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

  const char** video_services = VIDEO_SERVICES_PROD;
  printf("CommaView Bridge v3.3.8-safe-bundle (C++) [VIDEO+TELEMETRY][RAW_ONLY_DEFAULT][META_MODE=raw-only][EMIT_MS=%d]\n",
         g_telemetry_emit_ms);
  fflush(stdout);

  std::vector<std::pair<int, const char*>> streams;
  streams.push_back({PORT_ROAD, video_services[0]});
  streams.push_back({PORT_WIDE, video_services[1]});
  streams.push_back({PORT_DRIVER, video_services[2]});

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

  printf("ready. waiting for connections...\n");
  fflush(stdout);

  for (auto& t : threads) t.join();
  for (int fd : server_fds) close(fd);

  printf("CommaView Bridge stopped.\n");
  return 0;
}
#ifndef COMMAVIEW_BRIDGE_NO_MAIN
int main(int argc, char* argv[]) {
  return commaview_bridge_main(argc, argv);
}
#endif
