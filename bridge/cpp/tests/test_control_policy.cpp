#include "commaview/control/policy.h"
#include "commaview/net/framing.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

int main() {
  int fds[2]{};
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  const std::string sid = "session-test-123";
  const std::string json = "{\"op\":\"set_policy\",\"sessionId\":\"" + sid + "\",\"suppressVideo\":true}";

  std::vector<uint8_t> payload(1 + json.size());
  payload[0] = 0x03;  // MSG_CONTROL
  for (size_t i = 0; i < json.size(); i++) payload[1 + i] = static_cast<uint8_t>(json[i]);

  uint8_t hdr[4]{};
  commaview::net::put_be32(hdr, static_cast<uint32_t>(payload.size()));
  assert(write(fds[0], hdr, 4) == 4);
  assert(write(fds[0], payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));

  commaview::control::ClientControlState st;
  commaview::control::consume_client_control_frames(fds[1], &st, "test", 0x03);

  bool suppress = false;
  assert(commaview::control::get_session_policy(sid, &suppress));
  assert(suppress == true);


  using commaview::control::TailscalePolicyAction;
  assert(commaview::control::decide_tailscale_action(true, true) == TailscalePolicyAction::kForceDown);
  assert(commaview::control::decide_tailscale_action(true, false) == TailscalePolicyAction::kForceDown);
  assert(commaview::control::decide_tailscale_action(false, true) == TailscalePolicyAction::kEnsureUp);
  assert(commaview::control::decide_tailscale_action(false, false) == TailscalePolicyAction::kStayDown);

  close(fds[0]);
  close(fds[1]);
  return 0;
}
