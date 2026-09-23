#include "framing.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct ScriptedSendCall {
  ssize_t result = 0;
  int error = 0;
};

struct ScriptedSender {
  std::vector<ScriptedSendCall> calls;
  size_t call_index = 0;
  std::vector<uint8_t> bytes;
};

ssize_t scripted_send(void* ctx, int, const uint8_t* data, size_t len, int) {
  auto* sender = static_cast<ScriptedSender*>(ctx);
  assert(sender != nullptr);
  assert(sender->call_index < sender->calls.size());
  const ScriptedSendCall call = sender->calls[sender->call_index++];
  if (call.result < 0) {
    errno = call.error;
    return -1;
  }
  const size_t n = static_cast<size_t>(call.result);
  assert(n <= len);
  sender->bytes.insert(sender->bytes.end(), data, data + n);
  return call.result;
}

}  // namespace

static void test_bounded_send_retries_eintr_and_partial_success() {
  std::vector<uint8_t> payload{0x10, 0x20, 0x30, 0x40, 0x50};
  ScriptedSender sender{{
      {-1, EINTR},
      {2, 0},
      {-1, EINTR},
      {3, 0},
  }};

  const auto result = commaview::net::send_all_for_test(
      -1,
      payload.data(),
      payload.size(),
      commaview::net::SendDeadline::already_expired_for_test(false),
      scripted_send,
      &sender);

  assert(result.status == commaview::net::SendStatus::Ok);
  assert(result.bytes_sent == payload.size());
  assert(sender.bytes == payload);
  assert(sender.call_index == sender.calls.size());
}

static void test_bounded_send_classifies_eagain_as_backpressure() {
  std::vector<uint8_t> payload{0x01, 0x02};
  ScriptedSender sender{{{-1, EAGAIN}}};

  const auto result = commaview::net::send_all_for_test(
      -1,
      payload.data(),
      payload.size(),
      commaview::net::SendDeadline::already_expired_for_test(false),
      scripted_send,
      &sender);

  assert(result.status == commaview::net::SendStatus::Backpressure);
  assert(result.error == EAGAIN);
  assert(result.bytes_sent == 0);
  assert(sender.call_index == sender.calls.size());
}

static void test_bounded_send_reports_partial_progress_before_backpressure() {
  std::vector<uint8_t> payload{0x7A, 0x7B, 0x7C};
  ScriptedSender sender{{{1, 0}, {-1, EAGAIN}}};

  const auto result = commaview::net::send_all_for_test(
      -1,
      payload.data(),
      payload.size(),
      commaview::net::SendDeadline::already_expired_for_test(false),
      scripted_send,
      &sender);

  assert(result.status == commaview::net::SendStatus::Backpressure);
  assert(result.error == EAGAIN);
  assert(result.bytes_sent == 1);
  assert(sender.bytes == std::vector<uint8_t>({payload[0]}));
  assert(sender.call_index == sender.calls.size());
}

static void test_bounded_send_classifies_ewouldblock_as_backpressure() {
  std::vector<uint8_t> payload{0x01, 0x02};
  ScriptedSender sender{{{-1, EWOULDBLOCK}}};

  const auto result = commaview::net::send_all_for_test(
      -1,
      payload.data(),
      payload.size(),
      commaview::net::SendDeadline::already_expired_for_test(false),
      scripted_send,
      &sender);

  assert(result.status == commaview::net::SendStatus::Backpressure);
  assert(result.error == EWOULDBLOCK);
  assert(result.bytes_sent == 0);
  assert(sender.call_index == sender.calls.size());
}

static void test_send_diagnostics_names_are_stable() {
  assert(std::string(commaview::net::send_status_name(commaview::net::SendStatus::Ok)) == "ok");
  assert(std::string(commaview::net::send_status_name(commaview::net::SendStatus::Backpressure)) == "backpressure");
  assert(std::string(commaview::net::send_status_name(commaview::net::SendStatus::Disconnected)) == "disconnected");
  assert(std::string(commaview::net::send_status_name(commaview::net::SendStatus::InvalidArgument)) == "invalid_argument");
  assert(commaview::net::send_error_name(EPIPE) == "EPIPE");
  assert(commaview::net::send_error_name(ECONNRESET) == "ECONNRESET");
  assert(commaview::net::send_error_name(ENOTCONN) == "ENOTCONN");
  assert(commaview::net::send_error_name(EAGAIN) == "EAGAIN");
  assert(commaview::net::send_error_name(0) == "none");
}

static void test_bounded_send_classifies_epipe_as_disconnected() {
  std::vector<uint8_t> payload{0x01, 0x02};
  ScriptedSender sender{{{-1, EPIPE}}};

  const auto result = commaview::net::send_all_for_test(
      -1,
      payload.data(),
      payload.size(),
      commaview::net::SendDeadline::already_expired_for_test(false),
      scripted_send,
      &sender);

  assert(result.status == commaview::net::SendStatus::Disconnected);
  assert(result.error == EPIPE);
  assert(result.bytes_sent == 0);
  assert(sender.call_index == sender.calls.size());
}

static void test_send_iov_handles_partial_iovec_boundaries() {
  uint8_t a[] = {0xA1, 0xA2};
  uint8_t b[] = {0xB1, 0xB2, 0xB3};
  uint8_t c[] = {0xC1};
  std::vector<commaview::net::SendBuffer> buffers{
      {a, sizeof(a)},
      {b, sizeof(b)},
      {c, sizeof(c)},
  };

  ScriptedSender sender{{
      {1, 0},
      {3, 0},
      {2, 0},
  }};

  const auto result = commaview::net::send_buffers_for_test(
      -1,
      buffers.data(),
      buffers.size(),
      commaview::net::SendDeadline::already_expired_for_test(false),
      scripted_send,
      &sender);

  const std::vector<uint8_t> expected{0xA1, 0xA2, 0xB1, 0xB2, 0xB3, 0xC1};
  assert(result.status == commaview::net::SendStatus::Ok);
  assert(sender.bytes == expected);
}

static size_t fill_socket_until_backpressure(int fd) {
  std::vector<uint8_t> filler(4096, 0xEE);
  size_t bytes_sent = 0;
  while (true) {
    const ssize_t n = ::send(fd, filler.data(), filler.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n <= 0) break;
    bytes_sent += static_cast<size_t>(n);
  }
  assert(errno == EAGAIN || errno == EWOULDBLOCK);
  return bytes_sent;
}

static std::thread drain_socket_after_delay(int fd) {
  return std::thread([fd] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::vector<uint8_t> drain(64 * 1024);
    for (int i = 0; i < 8; ++i) {
      const ssize_t n = ::recv(fd, drain.data(), drain.size(), 0);
      if (n <= 0) break;
    }
  });
}

static void close_socketpair_and_join(int fds[2], std::thread* drainer) {
  shutdown(fds[0], SHUT_RDWR);
  shutdown(fds[1], SHUT_RDWR);
  if (drainer != nullptr && drainer->joinable()) drainer->join();
  close(fds[0]);
  close(fds[1]);
}

static void test_bounded_send_waits_for_socket_writability_before_deadline() {
  int fds[2]{};
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  fill_socket_until_backpressure(fds[0]);
  auto drainer = drain_socket_after_delay(fds[1]);

  std::vector<uint8_t> payload{0x41, 0x42, 0x43, 0x44};
  const auto result = commaview::net::send_all_bounded(
      fds[0],
      payload.data(),
      payload.size(),
      commaview::net::SendDeadline::after_micros(200000));

  close_socketpair_and_join(fds, &drainer);

  assert(result.status == commaview::net::SendStatus::Ok);
  assert(result.bytes_sent == payload.size());
  assert(result.elapsed_micros >= 1000);
}

static void test_bounded_send_buffers_waits_for_socket_writability_before_deadline() {
  int fds[2]{};
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  fill_socket_until_backpressure(fds[0]);
  auto drainer = drain_socket_after_delay(fds[1]);

  uint8_t a[] = {0x51, 0x52};
  uint8_t b[] = {0x53, 0x54};
  commaview::net::SendBuffer buffers[] = {{a, sizeof(a)}, {b, sizeof(b)}};
  const auto result = commaview::net::send_buffers_bounded(
      fds[0],
      buffers,
      2,
      commaview::net::SendDeadline::after_micros(200000));

  close_socketpair_and_join(fds, &drainer);

  assert(result.status == commaview::net::SendStatus::Ok);
  assert(result.bytes_sent == sizeof(a) + sizeof(b));
  assert(result.elapsed_micros >= 1000);
}

static void test_partial_frame_send_recovers_without_breaking_framing() {
  int fds[2]{};
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  int sndbuf = 4096;
  assert(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) == 0);

  std::vector<uint8_t> payload(256 * 1024, 0x5A);
  const size_t filler_bytes = 0;
  std::vector<uint8_t> recovered_frame;
  std::thread drainer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::vector<uint8_t> buffer(64 * 1024);
    size_t received = 0;
    const size_t expected = filler_bytes + payload.size() + 4;
    recovered_frame.reserve(payload.size() + 4);
    while (received < expected) {
      const ssize_t n = ::recv(fds[1], buffer.data(), buffer.size(), 0);
      if (n <= 0) break;
      const size_t chunk_start = received;
      const size_t chunk_end = received + static_cast<size_t>(n);
      if (chunk_end > filler_bytes) {
        const size_t copy_start = std::max(chunk_start, filler_bytes) - chunk_start;
        recovered_frame.insert(recovered_frame.end(),
                               buffer.begin() + static_cast<std::ptrdiff_t>(copy_start),
                               buffer.begin() + n);
      }
      received += static_cast<size_t>(n);
    }
  });

  const auto result = commaview::net::send_frame_with_partial_recovery(
      fds[0],
      payload.data(),
      payload.size(),
      commaview::net::SendDeadline::after_micros(1000),
      500000);

  drainer.join();
  assert(result.status == commaview::net::SendStatus::Ok);
  assert(result.bytes_sent == payload.size() + 4);
  assert(result.partial_recovery_attempted);
  assert(result.partial_recovery_succeeded);
  assert(recovered_frame.size() == payload.size() + 4);
  assert(commaview::net::read_be32(recovered_frame.data()) == payload.size());
  assert(std::equal(payload.begin(), payload.end(), recovered_frame.begin() + 4));

  const std::vector<uint8_t> next_payload{0x11, 0x22, 0x33};
  assert(commaview::net::send_frame(fds[0], next_payload.data(), next_payload.size()));
  uint8_t next_frame[7]{};
  assert(::recv(fds[1], next_frame, sizeof(next_frame), MSG_WAITALL) == sizeof(next_frame));
  assert(commaview::net::read_be32(next_frame) == next_payload.size());
  assert(std::equal(next_payload.begin(), next_payload.end(), next_frame + 4));

  close_socketpair_and_join(fds, nullptr);
}

int main() {
  test_send_diagnostics_names_are_stable();
  test_bounded_send_retries_eintr_and_partial_success();
  test_bounded_send_classifies_eagain_as_backpressure();
  test_bounded_send_reports_partial_progress_before_backpressure();
  test_bounded_send_classifies_ewouldblock_as_backpressure();
  test_bounded_send_classifies_epipe_as_disconnected();
  test_send_iov_handles_partial_iovec_boundaries();
  test_bounded_send_waits_for_socket_writability_before_deadline();
  test_bounded_send_buffers_waits_for_socket_writability_before_deadline();
  test_partial_frame_send_recovers_without_breaking_framing();

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
