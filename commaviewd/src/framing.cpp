#include "framing.h"

#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace commaview::net {

void put_be32(uint8_t* buf, uint32_t val) {
  buf[0] = (val >> 24) & 0xFF;
  buf[1] = (val >> 16) & 0xFF;
  buf[2] = (val >> 8) & 0xFF;
  buf[3] = val & 0xFF;
}

uint32_t read_be32(const uint8_t* buf) {
  return (static_cast<uint32_t>(buf[0]) << 24) |
         (static_cast<uint32_t>(buf[1]) << 16) |
         (static_cast<uint32_t>(buf[2]) << 8) |
         static_cast<uint32_t>(buf[3]);
}

bool send_all(int fd, const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool send_frame(int fd, const uint8_t* payload, size_t payload_len) {
  uint8_t hdr[4];
  put_be32(hdr, static_cast<uint32_t>(payload_len));
  if (!send_all(fd, hdr, 4)) return false;
  return send_all(fd, payload, payload_len);
}

bool send_meta_bytes(int fd, const uint8_t* bytes, size_t bytes_len, uint8_t msg_type) {
  if (bytes == nullptr || bytes_len == 0) return true;
  std::vector<uint8_t> payload(1 + bytes_len);
  payload[0] = msg_type;
  memcpy(&payload[1], bytes, bytes_len);
  return send_frame(fd, payload.data(), payload.size());
}

bool send_meta_json(int fd, const std::string& json, uint8_t msg_meta_type) {
  return send_meta_bytes(fd,
                         reinterpret_cast<const uint8_t*>(json.data()),
                         json.size(),
                         msg_meta_type);
}

}  // namespace commaview::net
