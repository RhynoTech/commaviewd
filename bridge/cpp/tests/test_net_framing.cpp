#include "commaview/net/framing.h"

#include <cassert>
#include <cstdint>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

int main() {
  uint8_t b[4]{};
  commaview::net::put_be32(b, 0xA1B2C3D4u);
  assert(commaview::net::read_be32(b) == 0xA1B2C3D4u);

  int fds[2]{};
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  std::vector<uint8_t> payload{0x01, 0x02, 0x03, 0x04};
  assert(commaview::net::send_frame(fds[0], payload.data(), payload.size()));

  uint8_t hdr[4]{};
  assert(read(fds[1], hdr, 4) == 4);
  assert(commaview::net::read_be32(hdr) == payload.size());

  uint8_t got[4]{};
  assert(read(fds[1], got, 4) == 4);
  for (size_t i = 0; i < payload.size(); i++) assert(got[i] == payload[i]);

  close(fds[0]);
  close(fds[1]);
  return 0;
}
