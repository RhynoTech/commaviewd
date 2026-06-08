#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace commaview::video {

static constexpr uint8_t MSG_VIDEO_CHUNK = 0x06;
static constexpr uint8_t VIDEO_CHUNK_KEYFRAME = 1 << 0;
static constexpr uint8_t VIDEO_CHUNK_FIRST = 1 << 1;
static constexpr uint8_t VIDEO_CHUNK_FINAL = 1 << 2;
static constexpr size_t DEFAULT_VIDEO_CHUNK_BYTES = 16 * 1024;

struct VideoFrameForChunking {
  uint32_t sequence = 0;
  uint64_t timestamp_ns = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool is_keyframe = false;
  std::vector<uint8_t> codec_header;
  std::vector<uint8_t> data;
};

struct VideoChunk {
  uint32_t frame_sequence = 0;
  uint16_t chunk_index = 0;
  uint16_t chunk_count = 0;
  uint8_t flags = 0;
  bool is_keyframe = false;
  bool is_first = false;
  bool is_final = false;
  uint64_t timestamp_ns = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t codec_header_len = 0;
  uint32_t data_len = 0;
  uint32_t offset = 0;
  std::vector<uint8_t> bytes;
};

std::vector<VideoChunk> plan_video_chunks(const VideoFrameForChunking& frame, size_t chunk_bytes);
std::vector<uint8_t> encode_video_chunk_payload(const VideoChunk& chunk);
VideoChunk decode_video_chunk_payload_for_test(const std::vector<uint8_t>& payload);

}  // namespace commaview::video
