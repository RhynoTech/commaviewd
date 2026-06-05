#include "policy.h"
#include "framing.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

void send_control_frame(int fd, const std::string& json) {
  std::vector<uint8_t> payload(1 + json.size());
  payload[0] = 0x03;  // MSG_CONTROL
  for (size_t i = 0; i < json.size(); i++) payload[1 + i] = static_cast<uint8_t>(json[i]);

  uint8_t hdr[4]{};
  commaview::net::put_be32(hdr, static_cast<uint32_t>(payload.size()));
  assert(write(fd, hdr, 4) == 4);
  assert(write(fd, payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));
}

void consume_control_json(const std::string& json, commaview::control::ClientControlState* state) {
  int fds[2]{};
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  send_control_frame(fds[0], json);
  commaview::control::consume_client_control_frames(fds[1], state, "test", 0x03);

  close(fds[0]);
  close(fds[1]);
}

}  // namespace

int main() {
  commaview::control::ClientControlState legacy{};
  assert(legacy.transport_version == 1);
  assert(std::string(legacy.client_role) == "legacy");
  assert(legacy.telemetry_on_video);

  const std::string sid = "session-test-123";
  const std::string json = "{\"op\":\"set_policy\",\"sessionId\":\"" + sid + "\",\"suppressVideo\":true}";

  commaview::control::ClientControlState st;
  consume_control_json(json, &st);

  bool suppress = false;
  assert(commaview::control::get_session_policy(sid, &suppress));
  assert(suppress == true);

  commaview::control::ClientControlState video_state;
  consume_control_json("{\"op\":\"set_policy\",\"sessionId\":\"session-1\",\"suppressVideo\":false,\"transportVersion\":2,\"clientRole\":\"video\",\"telemetryOnVideo\":false}",
                       &video_state);
  assert(video_state.transport_version == 2);
  assert(std::string(video_state.client_role) == "video");
  assert(!video_state.telemetry_on_video);

  commaview::control::ClientControlState telemetry_state;
  consume_control_json("{\"op\":\"set_policy\",\"sessionId\":\"session-1\",\"suppressVideo\":true,\"transportVersion\":2,\"clientRole\":\"telemetry\"}",
                       &telemetry_state);
  assert(telemetry_state.transport_version == 2);
  assert(std::string(telemetry_state.client_role) == "telemetry");
  assert(!telemetry_state.telemetry_on_video);

  return 0;
}
