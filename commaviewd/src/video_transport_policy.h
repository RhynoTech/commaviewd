#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace commaview::video {

bool contains_hevc_idr(const uint8_t* data, size_t len);

struct PendingVideoFrame {
  uint64_t sequence = 0;
  bool is_keyframe = false;
  uint64_t created_at_ms = 0;
  uint64_t timestamp_ns = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> codec_header;
  std::vector<uint8_t> data;
};

class VideoFrameQueue {
 public:
  explicit VideoFrameQueue(size_t capacity);

  void push(PendingVideoFrame frame);
  void note_backpressure_without_partial_send();

  std::optional<PendingVideoFrame> pop_next();

  size_t size() const;
  uint64_t drop_count() const;
  uint64_t keyframe_wait_drop_count() const;
  uint64_t frame_abandon_count() const;
  size_t high_watermark() const;

 private:
  size_t capacity_ = 1;
  bool waiting_for_keyframe_ = false;
  uint64_t drop_count_ = 0;
  uint64_t keyframe_wait_drop_count_ = 0;
  uint64_t frame_abandon_count_ = 0;
  size_t high_watermark_ = 0;
  std::deque<PendingVideoFrame> frames_;
};

}  // namespace commaview::video
