#include "framing.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

ssize_t system_send(void*, int fd, const uint8_t* data, size_t len, int flags) {
  return ::send(fd, data, len, flags);
}

commaview::net::SendStatus classify_send_error(int error) {
  if (error == EAGAIN || error == EWOULDBLOCK) return commaview::net::SendStatus::Backpressure;
  if (error == EPIPE || error == ECONNRESET || error == ENOTCONN) return commaview::net::SendStatus::Disconnected;
  return commaview::net::SendStatus::Disconnected;
}

}  // namespace

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

SendDeadline::SendDeadline(uint64_t budget_micros, bool force_expired)
    : budget_micros_(budget_micros),
      force_expired_(force_expired),
      started_at_(std::chrono::steady_clock::now()) {}

SendDeadline SendDeadline::after_micros(uint64_t budget_micros) {
  return SendDeadline(budget_micros, false);
}

SendDeadline SendDeadline::already_expired_for_test(bool expired) {
  return SendDeadline(0, expired);
}

bool SendDeadline::expired() const {
  if (force_expired_) return true;
  if (budget_micros_ == 0) return false;
  return elapsed_micros() >= budget_micros_;
}

uint64_t SendDeadline::elapsed_micros() const {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started_at_).count());
}

SendResult send_all_for_test(int fd,
                             const void* data,
                             size_t len,
                             SendDeadline deadline,
                             SendForTest send_fn,
                             void* send_ctx) {
  SendResult result{};
  if (data == nullptr && len > 0) {
    result.status = SendStatus::InvalidArgument;
    result.error = EINVAL;
    return result;
  }
  if (send_fn == nullptr) {
    result.status = SendStatus::InvalidArgument;
    result.error = EINVAL;
    return result;
  }

  const auto* p = static_cast<const uint8_t*>(data);
  while (result.bytes_sent < len) {
    if (deadline.expired()) {
      result.status = SendStatus::Backpressure;
      result.error = EAGAIN;
      result.elapsed_micros = deadline.elapsed_micros();
      return result;
    }

    const ssize_t n = send_fn(send_ctx,
                              fd,
                              p + result.bytes_sent,
                              len - result.bytes_sent,
                              MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      result.bytes_sent += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      result.status = SendStatus::Disconnected;
      result.elapsed_micros = deadline.elapsed_micros();
      return result;
    }

    const int err = errno;
    if (err == EINTR) continue;
    result.status = classify_send_error(err);
    result.error = err;
    result.elapsed_micros = deadline.elapsed_micros();
    return result;
  }

  result.status = SendStatus::Ok;
  result.elapsed_micros = deadline.elapsed_micros();
  return result;
}

SendResult send_all_bounded(int fd, const void* data, size_t len, SendDeadline deadline) {
  return send_all_for_test(fd, data, len, deadline, system_send, nullptr);
}

SendResult send_frame_bounded(int fd, const uint8_t* payload, size_t payload_len, SendDeadline deadline) {
  uint8_t hdr[4];
  put_be32(hdr, static_cast<uint32_t>(payload_len));

  SendResult header_result = send_all_bounded(fd, hdr, sizeof(hdr), deadline);
  if (header_result.status != SendStatus::Ok) return header_result;

  SendResult payload_result = send_all_bounded(fd, payload, payload_len, deadline);
  payload_result.bytes_sent += header_result.bytes_sent;
  payload_result.elapsed_micros = deadline.elapsed_micros();
  return payload_result;
}

bool send_all(int fd, const void* data, size_t len) {
  return send_all_bounded(fd, data, len, SendDeadline::after_micros(0)).status == SendStatus::Ok;
}

bool send_frame(int fd, const uint8_t* payload, size_t payload_len) {
  return send_frame_bounded(fd, payload, payload_len, SendDeadline::after_micros(0)).status == SendStatus::Ok;
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
