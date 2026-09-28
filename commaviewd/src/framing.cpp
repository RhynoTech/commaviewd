#include "framing.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <poll.h>
#include <sys/socket.h>
#include <vector>

namespace {

using commaview::net::SendDeadline;
using commaview::net::SendResult;
using commaview::net::SendStatus;

constexpr int kSendFlags = MSG_NOSIGNAL | MSG_DONTWAIT;

SendResult invalid_argument_result() {
  SendResult result{};
  result.status = SendStatus::InvalidArgument;
  result.error = EINVAL;
  return result;
}

// EAGAIN/EWOULDBLOCK are backpressure; every other errno (EPIPE, ECONNRESET, ENOTCONN, ...)
// is treated as a disconnect.
SendStatus classify_send_error(int error) {
  if (error == EAGAIN || error == EWOULDBLOCK) return SendStatus::Backpressure;
  return SendStatus::Disconnected;
}

bool wait_for_socket_writable(int fd, const SendDeadline& deadline) {
  if (fd < 0) return false;
  while (!deadline.expired()) {
    const uint64_t remaining_us = deadline.remaining_micros();
    int timeout_ms = -1;
    if (remaining_us != std::numeric_limits<uint64_t>::max()) {
      timeout_ms = static_cast<int>(std::max<uint64_t>(1, std::min<uint64_t>(remaining_us / 1000, 100)));
    }

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int ready = poll(&pfd, 1, timeout_ms);
    if (ready > 0) return (pfd.revents & POLLOUT) != 0;
    if (ready == 0) continue;
    if (errno == EINTR) continue;
    return false;
  }
  return false;
}

// The bounded, non-blocking send loop shared by send_all_bounded and send_all_for_test.
// `send_once(data, len)` makes one send attempt over the unsent bytes. Only send_all_bounded
// passes wait_for_writable=true: on EAGAIN/EWOULDBLOCK it polls for POLLOUT until the deadline
// instead of returning Backpressure immediately.
template <typename SendOnce>
SendResult bounded_send_loop(int fd,
                             const uint8_t* data,
                             size_t len,
                             const SendDeadline& deadline,
                             bool wait_for_writable,
                             SendOnce send_once) {
  SendResult result{};
  while (result.bytes_sent < len) {
    if (deadline.expired()) {
      result.status = SendStatus::Backpressure;
      result.error = EAGAIN;
      result.elapsed_micros = deadline.elapsed_micros();
      return result;
    }

    const ssize_t n = send_once(data + result.bytes_sent, len - result.bytes_sent);
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
    if (wait_for_writable) {
      if (result.status == SendStatus::Backpressure && wait_for_socket_writable(fd, deadline)) {
        result.status = SendStatus::Ok;
        result.error = 0;
        continue;
      }
      result.elapsed_micros = deadline.elapsed_micros();
    }
    return result;
  }

  result.status = SendStatus::Ok;
  result.elapsed_micros = deadline.elapsed_micros();
  return result;
}

}  // namespace

namespace commaview::net {

const char* send_status_name(SendStatus status) {
  switch (status) {
    case SendStatus::Ok:
      return "ok";
    case SendStatus::Backpressure:
      return "backpressure";
    case SendStatus::Disconnected:
      return "disconnected";
    case SendStatus::InvalidArgument:
      return "invalid_argument";
  }
  return "unknown";
}

std::string send_error_name(int error) {
  if (error == 0) return "none";
  if (error == EPIPE) return "EPIPE";
  if (error == ECONNRESET) return "ECONNRESET";
  if (error == ENOTCONN) return "ENOTCONN";
  if (error == EAGAIN) return "EAGAIN";
  if (error == EWOULDBLOCK) return "EWOULDBLOCK";
  if (error == EINTR) return "EINTR";
  if (error == EINVAL) return "EINVAL";
  return std::string("errno_") + std::to_string(error);
}

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

SendDeadline::SendDeadline(uint64_t budget_micros)
    : budget_micros_(budget_micros),
      started_at_(std::chrono::steady_clock::now()) {}

SendDeadline SendDeadline::after_micros(uint64_t budget_micros) {
  return SendDeadline(budget_micros);
}

bool SendDeadline::expired() const {
  if (budget_micros_ == 0) return false;
  return elapsed_micros() >= budget_micros_;
}

uint64_t SendDeadline::elapsed_micros() const {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started_at_).count());
}

uint64_t SendDeadline::remaining_micros() const {
  if (budget_micros_ == 0) return std::numeric_limits<uint64_t>::max();
  const uint64_t elapsed = elapsed_micros();
  return elapsed >= budget_micros_ ? 0 : budget_micros_ - elapsed;
}

SendResult send_all_for_test(int fd,
                             const void* data,
                             size_t len,
                             SendDeadline deadline,
                             SendForTest send_fn,
                             void* send_ctx) {
  if (data == nullptr && len > 0) return invalid_argument_result();
  if (send_fn == nullptr) return invalid_argument_result();
  return bounded_send_loop(fd, static_cast<const uint8_t*>(data), len, deadline, /*wait_for_writable=*/false,
                           [&](const uint8_t* p, size_t n) { return send_fn(send_ctx, fd, p, n, kSendFlags); });
}

SendResult send_all_bounded(int fd, const void* data, size_t len, SendDeadline deadline) {
  if (data == nullptr && len > 0) return invalid_argument_result();
  return bounded_send_loop(fd, static_cast<const uint8_t*>(data), len, deadline, /*wait_for_writable=*/true,
                           [fd](const uint8_t* p, size_t n) { return ::send(fd, p, n, kSendFlags); });
}

SendResult send_frame_with_partial_recovery(int fd,
                                            const uint8_t* payload,
                                            size_t payload_len,
                                            SendDeadline initial_deadline,
                                            uint64_t recovery_budget_micros) {
  if (payload == nullptr && payload_len > 0) return invalid_argument_result();

  std::vector<uint8_t> frame(4 + payload_len);
  put_be32(frame.data(), static_cast<uint32_t>(payload_len));
  if (payload_len > 0) memcpy(frame.data() + 4, payload, payload_len);

  SendResult result = send_all_bounded(fd, frame.data(), frame.size(), initial_deadline);
  if (result.status != SendStatus::Backpressure || result.bytes_sent == 0 ||
      result.bytes_sent >= frame.size()) {
    return result;
  }

  const size_t initial_bytes = result.bytes_sent;
  const uint64_t initial_elapsed = result.elapsed_micros;
  SendResult recovery = send_all_bounded(
      fd,
      frame.data() + initial_bytes,
      frame.size() - initial_bytes,
      SendDeadline::after_micros(recovery_budget_micros));
  recovery.bytes_sent += initial_bytes;
  recovery.elapsed_micros += initial_elapsed;
  recovery.partial_recovery_attempted = true;
  recovery.partial_recovery_succeeded = recovery.status == SendStatus::Ok;
  return recovery;
}

}  // namespace commaview::net
