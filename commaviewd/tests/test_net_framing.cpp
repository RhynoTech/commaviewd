#include "framing.h"

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <sys/socket.h>
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

int main() {
  test_send_diagnostics_names_are_stable();
  test_bounded_send_retries_eintr_and_partial_success();
  test_bounded_send_classifies_eagain_as_backpressure();
  test_bounded_send_reports_partial_progress_before_backpressure();
  test_bounded_send_classifies_ewouldblock_as_backpressure();
  test_bounded_send_classifies_epipe_as_disconnected();
  test_send_iov_handles_partial_iovec_boundaries();

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
