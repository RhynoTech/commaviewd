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
  bool partial_recovery_attempted = false;
  bool partial_recovery_succeeded = false;
};

const char* send_status_name(SendStatus status);
std::string send_error_name(int error);

class SendDeadline {
 public:
  // A budget of 0 means no deadline.
  static SendDeadline after_micros(uint64_t budget_micros);

  bool expired() const;
  uint64_t elapsed_micros() const;
  uint64_t remaining_micros() const;

 private:
  explicit SendDeadline(uint64_t budget_micros);
  uint64_t budget_micros_ = 0;
  std::chrono::steady_clock::time_point started_at_;
};

using SendForTest = ssize_t (*)(void* ctx, int fd, const uint8_t* data, size_t len, int flags);

SendResult send_all_bounded(int fd, const void* data, size_t len, SendDeadline deadline);
SendResult send_frame_with_partial_recovery(int fd,
                                            const uint8_t* payload,
                                            size_t payload_len,
                                            SendDeadline initial_deadline,
                                            uint64_t recovery_budget_micros);

// Test seam: the send_all_bounded loop with an injected send function and without the wait
// for POLLOUT. The bridge sends through send_frame_with_partial_recovery.
SendResult send_all_for_test(int fd,
                             const void* data,
                             size_t len,
                             SendDeadline deadline,
                             SendForTest send_fn,
                             void* send_ctx);

}  // namespace commaview::net
