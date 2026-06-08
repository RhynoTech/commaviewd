#include "video_chunk_protocol.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <utility>

namespace commaview::video {
namespace {

void append_u16_be(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>(value & 0xff));
}

void append_u32_be(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>(value & 0xff));
}

void append_u64_be(std::vector<uint8_t>& out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
  }
}

#ifdef COMMAVIEW_VIDEO_CHUNK_PROTOCOL_TESTING
uint16_t read_u16_be(const std::vector<uint8_t>& payload, size_t* pos) {
  assert(*pos + 2 <= payload.size());
  const uint16_t value = static_cast<uint16_t>((static_cast<uint16_t>(payload[*pos]) << 8) |
                                               static_cast<uint16_t>(payload[*pos + 1]));
  *pos += 2;
  return value;
}

uint32_t read_u32_be(const std::vector<uint8_t>& payload, size_t* pos) {
  assert(*pos + 4 <= payload.size());
  const uint32_t value = (static_cast<uint32_t>(payload[*pos]) << 24) |
                         (static_cast<uint32_t>(payload[*pos + 1]) << 16) |
                         (static_cast<uint32_t>(payload[*pos + 2]) << 8) |
                         static_cast<uint32_t>(payload[*pos + 3]);
  *pos += 4;
  return value;
}

uint64_t read_u64_be(const std::vector<uint8_t>& payload, size_t* pos) {
  assert(*pos + 8 <= payload.size());
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<uint64_t>(payload[*pos + i]);
  }
  *pos += 8;
  return value;
}
#endif

uint8_t chunk_flags(bool is_keyframe, bool is_first, bool is_final) {
  uint8_t flags = 0;
  if (is_keyframe) flags |= VIDEO_CHUNK_KEYFRAME;
  if (is_first) flags |= VIDEO_CHUNK_FIRST;
  if (is_final) flags |= VIDEO_CHUNK_FINAL;
  return flags;
}

size_t checked_add_size(size_t lhs, size_t rhs, const char* message) {
  if (rhs > std::numeric_limits<size_t>::max() - lhs) {
    throw std::length_error(message);
  }
  return lhs + rhs;
}

struct VideoChunkLayout {
  size_t logical_size = 0;
  size_t chunk_bytes = 0;
  size_t chunk_count = 0;
};

VideoChunkLayout plan_video_chunk_layout(size_t codec_header_size, size_t data_size, size_t chunk_bytes) {
  const size_t max_chunk_bytes = chunk_bytes == 0 ? DEFAULT_VIDEO_CHUNK_BYTES : chunk_bytes;
  const size_t logical_size = checked_add_size(codec_header_size, data_size,
                                              "video frame is too large to chunk");
  if (logical_size == 0) {
    throw std::invalid_argument("video frame must not be empty");
  }
  if (codec_header_size > std::numeric_limits<uint32_t>::max() ||
      data_size > std::numeric_limits<uint32_t>::max() ||
      logical_size > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("video frame is too large to chunk");
  }
  const size_t chunk_count_size = 1 + ((logical_size - 1) / max_chunk_bytes);
  if (chunk_count_size > std::numeric_limits<uint16_t>::max()) {
    throw std::length_error("video frame has too many chunks");
  }
  return {logical_size, max_chunk_bytes, chunk_count_size};
}

}  // namespace

std::vector<VideoChunk> plan_video_chunks(const VideoFrameForChunking& frame, size_t chunk_bytes) {
  const VideoChunkLayout layout = plan_video_chunk_layout(frame.codec_header.size(), frame.data.size(), chunk_bytes);
  const size_t logical_size = layout.logical_size;
  const size_t max_chunk_bytes = layout.chunk_bytes;
  const size_t chunk_count_size = layout.chunk_count;

  std::vector<uint8_t> logical_frame;
  logical_frame.reserve(logical_size);
  logical_frame.insert(logical_frame.end(), frame.codec_header.begin(), frame.codec_header.end());
  logical_frame.insert(logical_frame.end(), frame.data.begin(), frame.data.end());

  std::vector<VideoChunk> chunks;
  chunks.reserve(chunk_count_size);
  size_t offset = 0;
  for (size_t index = 0; index < chunk_count_size; ++index) {
    const size_t remaining = logical_frame.size() - offset;
    const size_t len = std::min(max_chunk_bytes, remaining);

    VideoChunk chunk;
    chunk.frame_sequence = frame.sequence;
    chunk.chunk_index = static_cast<uint16_t>(index);
    chunk.chunk_count = static_cast<uint16_t>(chunk_count_size);
    chunk.is_keyframe = frame.is_keyframe;
    chunk.is_first = index == 0;
    chunk.is_final = index + 1 == chunk_count_size;
    chunk.timestamp_ns = frame.timestamp_ns;
    chunk.width = frame.width;
    chunk.height = frame.height;
    chunk.codec_header_len = static_cast<uint32_t>(frame.codec_header.size());
    chunk.data_len = static_cast<uint32_t>(frame.data.size());
    chunk.offset = static_cast<uint32_t>(offset);
    chunk.bytes.assign(logical_frame.begin() + static_cast<std::ptrdiff_t>(offset),
                       logical_frame.begin() + static_cast<std::ptrdiff_t>(offset + len));
    chunks.push_back(std::move(chunk));
    offset += len;
  }

  return chunks;
}

std::vector<uint8_t> encode_video_chunk_payload(const VideoChunk& chunk) {
  if (chunk.bytes.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("video chunk is too large to encode");
  }

  std::vector<uint8_t> payload;
  payload.reserve(1 + 4 + 2 + 2 + 1 + 8 + 4 + 4 + 4 + 4 + 4 + 4 + chunk.bytes.size());
  payload.push_back(MSG_VIDEO_CHUNK);
  append_u32_be(payload, chunk.frame_sequence);
  append_u16_be(payload, chunk.chunk_index);
  append_u16_be(payload, chunk.chunk_count);
  payload.push_back(chunk_flags(chunk.is_keyframe, chunk.is_first, chunk.is_final));
  append_u64_be(payload, chunk.timestamp_ns);
  append_u32_be(payload, chunk.width);
  append_u32_be(payload, chunk.height);
  append_u32_be(payload, chunk.codec_header_len);
  append_u32_be(payload, chunk.data_len);
  append_u32_be(payload, chunk.offset);
  append_u32_be(payload, static_cast<uint32_t>(chunk.bytes.size()));
  payload.insert(payload.end(), chunk.bytes.begin(), chunk.bytes.end());
  return payload;
}

#ifdef COMMAVIEW_VIDEO_CHUNK_PROTOCOL_TESTING
VideoChunkLayoutForTest plan_video_chunk_layout_for_test(size_t codec_header_size, size_t data_size,
                                                         size_t chunk_bytes) {
  const VideoChunkLayout layout = plan_video_chunk_layout(codec_header_size, data_size, chunk_bytes);
  return {layout.logical_size, layout.chunk_bytes, static_cast<uint16_t>(layout.chunk_count)};
}

VideoChunk decode_video_chunk_payload_for_test(const std::vector<uint8_t>& payload) {
  assert(!payload.empty());
  size_t pos = 0;
  assert(payload[pos++] == MSG_VIDEO_CHUNK);

  VideoChunk chunk;
  chunk.frame_sequence = read_u32_be(payload, &pos);
  chunk.chunk_index = read_u16_be(payload, &pos);
  chunk.chunk_count = read_u16_be(payload, &pos);
  assert(pos < payload.size());
  const uint8_t flags = payload[pos++];
  chunk.is_keyframe = (flags & VIDEO_CHUNK_KEYFRAME) != 0;
  chunk.is_first = (flags & VIDEO_CHUNK_FIRST) != 0;
  chunk.is_final = (flags & VIDEO_CHUNK_FINAL) != 0;
  chunk.timestamp_ns = read_u64_be(payload, &pos);
  chunk.width = read_u32_be(payload, &pos);
  chunk.height = read_u32_be(payload, &pos);
  chunk.codec_header_len = read_u32_be(payload, &pos);
  chunk.data_len = read_u32_be(payload, &pos);
  chunk.offset = read_u32_be(payload, &pos);
  const uint32_t chunk_len = read_u32_be(payload, &pos);
  assert(pos + chunk_len == payload.size());
  chunk.bytes.assign(payload.begin() + static_cast<std::ptrdiff_t>(pos), payload.end());
  return chunk;
}
#endif

}  // namespace commaview::video
