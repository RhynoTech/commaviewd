#include "framing.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits.h>
#include <sys/socket.h>
#include <sys/uio.h>
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

bool validate_send_buffers(const commaview::net::SendBuffer* buffers,
                           size_t buffer_count,
                           commaview::net::SendResult* result) {
  if (buffers == nullptr && buffer_count > 0) {
    result->status = commaview::net::SendStatus::InvalidArgument;
    result->error = EINVAL;
    return false;
  }
  for (size_t i = 0; i < buffer_count; ++i) {
    if (buffers[i].data == nullptr && buffers[i].len > 0) {
      result->status = commaview::net::SendStatus::InvalidArgument;
      result->error = EINVAL;
      return false;
    }
  }
  return true;
}

std::vector<iovec> build_iovecs(const commaview::net::SendBuffer* buffers, size_t buffer_count) {
  std::vector<iovec> iovecs;
  iovecs.reserve(buffer_count);
  for (size_t i = 0; i < buffer_count; ++i) {
    if (buffers[i].len == 0) continue;
    iovec iov{};
    iov.iov_base = const_cast<uint8_t*>(buffers[i].data);
    iov.iov_len = buffers[i].len;
    iovecs.push_back(iov);
  }
  return iovecs;
}

void advance_iovecs(std::vector<iovec>* iovecs, size_t* iov_index, size_t bytes) {
  while (bytes > 0 && *iov_index < iovecs->size()) {
    iovec& iov = (*iovecs)[*iov_index];
    if (bytes < iov.iov_len) {
      iov.iov_base = static_cast<uint8_t*>(iov.iov_base) + bytes;
      iov.iov_len -= bytes;
      return;
    }
    bytes -= iov.iov_len;
    iov.iov_len = 0;
    ++(*iov_index);
  }
}

size_t max_iov_per_sendmsg() {
#ifdef IOV_MAX
  return static_cast<size_t>(IOV_MAX);
#else
  const long value = sysconf(_SC_IOV_MAX);
  return value > 0 ? static_cast<size_t>(value) : 16U;
#endif
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

SendResult send_buffers_for_test(int fd,
                                 const SendBuffer* buffers,
                                 size_t buffer_count,
                                 SendDeadline deadline,
                                 SendForTest send_fn,
                                 void* send_ctx) {
  SendResult result{};
  if (!validate_send_buffers(buffers, buffer_count, &result)) return result;
  if (send_fn == nullptr) {
    result.status = SendStatus::InvalidArgument;
    result.error = EINVAL;
    return result;
  }

  std::vector<iovec> iovecs = build_iovecs(buffers, buffer_count);
  size_t iov_index = 0;
  while (iov_index < iovecs.size()) {
    if (deadline.expired()) {
      result.status = SendStatus::Backpressure;
      result.error = EAGAIN;
      result.elapsed_micros = deadline.elapsed_micros();
      return result;
    }

    std::vector<uint8_t> remaining;
    for (size_t i = iov_index; i < iovecs.size(); ++i) {
      const auto* base = static_cast<const uint8_t*>(iovecs[i].iov_base);
      remaining.insert(remaining.end(), base, base + iovecs[i].iov_len);
    }

    const ssize_t n = send_fn(send_ctx,
                              fd,
                              remaining.data(),
                              remaining.size(),
                              MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      result.bytes_sent += static_cast<size_t>(n);
      advance_iovecs(&iovecs, &iov_index, static_cast<size_t>(n));
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

SendResult send_buffers_bounded(int fd,
                                const SendBuffer* buffers,
                                size_t buffer_count,
                                SendDeadline deadline) {
  SendResult result{};
  if (!validate_send_buffers(buffers, buffer_count, &result)) return result;

  std::vector<iovec> iovecs = build_iovecs(buffers, buffer_count);
  size_t iov_index = 0;
  const size_t max_iov = max_iov_per_sendmsg();
  while (iov_index < iovecs.size()) {
    if (deadline.expired()) {
      result.status = SendStatus::Backpressure;
      result.error = EAGAIN;
      result.elapsed_micros = deadline.elapsed_micros();
      return result;
    }

    msghdr msg{};
    msg.msg_iov = &iovecs[iov_index];
    msg.msg_iovlen = std::min(max_iov, iovecs.size() - iov_index);

    const ssize_t n = ::sendmsg(fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      result.bytes_sent += static_cast<size_t>(n);
      advance_iovecs(&iovecs, &iov_index, static_cast<size_t>(n));
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

SendResult send_frame_bounded(int fd, const uint8_t* payload, size_t payload_len, SendDeadline deadline) {
  uint8_t hdr[4];
  put_be32(hdr, static_cast<uint32_t>(payload_len));
  const SendBuffer buffers[] = {
      {hdr, sizeof(hdr)},
      {payload, payload_len},
  };
  return send_buffers_bounded(fd, buffers, 2, deadline);
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
