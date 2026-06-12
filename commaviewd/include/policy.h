#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace commaview::control {

struct ClientControlState {
  std::vector<uint8_t> rx_buf;
  std::string bound_session_id;
  int transport_version = 1;
  std::string client_role = "legacy";
  bool telemetry_on_video = true;
  // "full" (default) or "udp_snapshot": the client receives overlay telemetry
  // via lossy UDP snapshots, so Pass-mode services may be demoted to a low TCP
  // keepalive rate (alert-bearing services always stay full-rate).
  std::string telemetry_transport = "full";
  uint64_t control_update_count = 0;
  uint64_t parse_error_count = 0;
};

void set_session_policy(const std::string& session_id, bool suppress_video);
bool get_session_policy(const std::string& session_id, bool* suppress_video);

void consume_client_control_frames(int client_fd,
                                   ClientControlState* state,
                                   const char* video_service,
                                   uint8_t msg_control_type);


}  // namespace commaview::control
