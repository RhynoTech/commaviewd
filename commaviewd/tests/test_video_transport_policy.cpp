#include "video_transport_policy.h"

#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

std::vector<uint8_t> annex_b_nal(uint8_t nal_type) {
  return {0x00, 0x00, 0x00, 0x01, static_cast<uint8_t>(nal_type << 1), 0x01, 0xaa, 0xbb};
}

commaview::video::PendingVideoFrame frame(uint64_t sequence, bool keyframe) {
  commaview::video::PendingVideoFrame pending;
  pending.sequence = sequence;
  pending.is_keyframe = keyframe;
  pending.payload_bytes = 100;
  pending.created_at_ms = sequence * 10;
  return pending;
}

void test_detects_idr_w_radl() {
  const auto payload = annex_b_nal(19);
  assert(commaview::video::contains_hevc_idr(payload.data(), payload.size()));
}

void test_detects_idr_n_lp() {
  const auto payload = annex_b_nal(20);
  assert(commaview::video::contains_hevc_idr(payload.data(), payload.size()));
}

void test_non_idr_is_not_keyframe() {
  const auto payload = annex_b_nal(1);
  assert(!commaview::video::contains_hevc_idr(payload.data(), payload.size()));
}

void test_truncated_hevc_nal_header_is_not_keyframe() {
  const std::vector<uint8_t> payload = {0x00, 0x00, 0x00, 0x01, static_cast<uint8_t>(19 << 1)};
  assert(!commaview::video::contains_hevc_idr(payload.data(), payload.size()));
}

void test_queue_keeps_latest_when_full() {
  commaview::video::VideoFrameQueue queue(2);
  queue.push(frame(1, true));
  queue.push(frame(2, false));
  queue.push(frame(3, false));

  assert(queue.size() == 2);
  assert(queue.drop_count() == 1);
  const auto next = queue.pop_next();
  assert(next.has_value());
  assert(next->sequence == 2);
}

void test_pressure_waits_for_keyframe_after_drops() {
  commaview::video::VideoFrameQueue queue(3);
  queue.note_backpressure_without_partial_send();
  queue.push(frame(1, false));
  queue.push(frame(2, false));

  assert(!queue.pop_next().has_value());
  assert(queue.keyframe_wait_drop_count() == 2);

  queue.push(frame(3, true));
  const auto next = queue.pop_next();
  assert(next.has_value());
  assert(next->sequence == 3);
  assert(next->is_keyframe);
}

void test_zero_byte_chunk_backpressure_abandons_frame_and_requires_keyframe_resume() {
  commaview::video::VideoFrameQueue queue(4);
  queue.push(frame(1, false));
  auto first = queue.pop_next();
  assert(first.has_value());
  assert(first->sequence == 1);

  queue.note_backpressure_without_partial_send();
  assert(queue.frame_abandon_count() == 1);

  queue.push(frame(2, false));
  assert(!queue.pop_next().has_value());
  assert(queue.keyframe_wait_drop_count() == 1);

  queue.push(frame(3, true));
  auto resumed = queue.pop_next();
  assert(resumed.has_value());
  assert(resumed->sequence == 3);
  assert(resumed->is_keyframe);
}

}  // namespace

int main() {
  test_detects_idr_w_radl();
  test_detects_idr_n_lp();
  test_non_idr_is_not_keyframe();
  test_truncated_hevc_nal_header_is_not_keyframe();
  test_queue_keeps_latest_when_full();
  test_pressure_waits_for_keyframe_after_drops();
  test_zero_byte_chunk_backpressure_abandons_frame_and_requires_keyframe_resume();
  return 0;
}
