#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>

namespace commaview::net {

void put_be32(uint8_t* buf, uint32_t val);
uint32_t read_be32(const uint8_t* buf);

enum class SendStatus {
  Ok,
  Backpressure,
  Disconnected,
  InvalidArgument,
};

struct SendResult {
  SendStatus status = SendStatus::Ok;
  size_t bytes_sent = 0;
  int error = 0;
  uint64_t elapsed_micros = 0;
};

const char* send_status_name(SendStatus status);
std::string send_error_name(int error);

class SendDeadline {
 public:
  static SendDeadline after_micros(uint64_t budget_micros);
  static SendDeadline already_expired_for_test(bool expired);

  bool expired() const;
  uint64_t elapsed_micros() const;
  uint64_t remaining_micros() const;

 private:
  explicit SendDeadline(uint64_t budget_micros, bool force_expired = false);
  uint64_t budget_micros_ = 0;
  bool force_expired_ = false;
  std::chrono::steady_clock::time_point started_at_;
};

using SendForTest = ssize_t (*)(void* ctx, int fd, const uint8_t* data, size_t len, int flags);

struct SendBuffer {
  const uint8_t* data = nullptr;
  size_t len = 0;
};

SendResult send_all_bounded(int fd, const void* data, size_t len, SendDeadline deadline);
SendResult send_frame_bounded(int fd, const uint8_t* payload, size_t payload_len, SendDeadline deadline);
SendResult send_buffers_bounded(int fd,
                                const SendBuffer* buffers,
                                size_t buffer_count,
                                SendDeadline deadline);

// Test seams. Production callers should use send_all_bounded/send_frame_bounded/send_buffers_bounded.
SendResult send_all_for_test(int fd,
                             const void* data,
                             size_t len,
                             SendDeadline deadline,
                             SendForTest send_fn,
                             void* send_ctx);
SendResult send_buffers_for_test(int fd,
                                 const SendBuffer* buffers,
                                 size_t buffer_count,
                                 SendDeadline deadline,
                                 SendForTest send_fn,
                                 void* send_ctx);

bool send_all(int fd, const void* data, size_t len);
bool send_frame(int fd, const uint8_t* payload, size_t payload_len);
bool send_meta_bytes(int fd, const uint8_t* bytes, size_t bytes_len, uint8_t msg_type);
bool send_meta_json(int fd, const std::string& json, uint8_t msg_meta_type);

}  // namespace commaview::net
