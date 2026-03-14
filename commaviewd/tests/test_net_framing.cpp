#include "framing.h"

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

  // Basic frame
  std::vector<uint8_t> payload{0x01, 0x02, 0x03, 0x04};
  assert(commaview::net::send_frame(fds[0], payload.data(), payload.size()));

  uint8_t hdr[4]{};
  assert(read(fds[1], hdr, 4) == 4);
  assert(commaview::net::read_be32(hdr) == payload.size());

  uint8_t got[4]{};
  assert(read(fds[1], got, 4) == 4);
  for (size_t i = 0; i < payload.size(); i++) assert(got[i] == payload[i]);

  // Meta bytes frame
  std::vector<uint8_t> meta{0xAA, 0xBB, 0xCC};
  constexpr uint8_t msg_type = 0x04;
  assert(commaview::net::send_meta_bytes(fds[0], meta.data(), meta.size(), msg_type));

  assert(read(fds[1], hdr, 4) == 4);
  assert(commaview::net::read_be32(hdr) == meta.size() + 1);

  uint8_t meta_got[4]{};
  assert(read(fds[1], meta_got, 4) == 4);
  assert(meta_got[0] == msg_type);
  assert(meta_got[1] == 0xAA);
  assert(meta_got[2] == 0xBB);
  assert(meta_got[3] == 0xCC);

  close(fds[0]);
  close(fds[1]);
  return 0;
}
