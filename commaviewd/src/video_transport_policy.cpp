#include "video_transport_policy.h"

#include <algorithm>
#include <utility>

namespace commaview::video {
namespace {

bool is_start_code(const uint8_t* data, size_t len, size_t pos, size_t* start_code_len) {
  if (pos + 3 <= len && data[pos] == 0x00 && data[pos + 1] == 0x00 && data[pos + 2] == 0x01) {
    *start_code_len = 3;
    return true;
  }
  if (pos + 4 <= len &&
      data[pos] == 0x00 &&
      data[pos + 1] == 0x00 &&
      data[pos + 2] == 0x00 &&
      data[pos + 3] == 0x01) {
    *start_code_len = 4;
    return true;
  }
  return false;
}

}  // namespace

bool contains_hevc_idr(const uint8_t* data, size_t len) {
  if (data == nullptr || len < 5) return false;

  for (size_t pos = 0; pos + 5 <= len; ++pos) {
    size_t start_code_len = 0;
    if (!is_start_code(data, len, pos, &start_code_len)) continue;
    const size_t nal_header = pos + start_code_len;
    if (nal_header + 1 >= len) continue;
    const uint8_t nal_type = static_cast<uint8_t>((data[nal_header] >> 1) & 0x3f);
    if (nal_type == 19 || nal_type == 20) return true;
  }

  return false;
}

VideoFrameQueue::VideoFrameQueue(size_t capacity)
    : capacity_(std::max<size_t>(capacity, 1)) {}

void VideoFrameQueue::push(PendingVideoFrame frame) {
  while (frames_.size() >= capacity_) {
    frames_.pop_front();
    drop_count_ += 1;
  }
  frames_.push_back(std::move(frame));
  high_watermark_ = std::max(high_watermark_, frames_.size());
}

void VideoFrameQueue::note_backpressure_without_partial_send() {
  waiting_for_keyframe_ = true;
  frame_abandon_count_ += 1;
}

std::optional<PendingVideoFrame> VideoFrameQueue::pop_next() {
  if (waiting_for_keyframe_) {
    while (!frames_.empty() && !frames_.front().is_keyframe) {
      frames_.pop_front();
      keyframe_wait_drop_count_ += 1;
      drop_count_ += 1;
    }
    if (frames_.empty()) return std::nullopt;
    waiting_for_keyframe_ = false;
  }

  if (frames_.empty()) return std::nullopt;
  auto frame = std::move(frames_.front());
  frames_.pop_front();
  return frame;
}

size_t VideoFrameQueue::size() const {
  return frames_.size();
}

uint64_t VideoFrameQueue::drop_count() const {
  return drop_count_;
}

uint64_t VideoFrameQueue::keyframe_wait_drop_count() const {
  return keyframe_wait_drop_count_;
}

uint64_t VideoFrameQueue::frame_abandon_count() const {
  return frame_abandon_count_;
}

size_t VideoFrameQueue::high_watermark() const {
  return high_watermark_;
}

}  // namespace commaview::video
