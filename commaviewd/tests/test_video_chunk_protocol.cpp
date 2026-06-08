#include "video_chunk_protocol.h"

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void assert_throws_invalid_argument(const char* expected, void (*fn)()) {
  try {
    fn();
    assert(false && "expected invalid_argument");
  } catch (const std::invalid_argument& ex) {
    assert(std::string(ex.what()).find(expected) != std::string::npos);
  }
}

void assert_throws_length_error(const char* expected, void (*fn)()) {
  try {
    fn();
    assert(false && "expected length_error");
  } catch (const std::length_error& ex) {
    assert(std::string(ex.what()).find(expected) != std::string::npos);
  }
}

void test_single_chunk_frame() {
  commaview::video::VideoFrameForChunking frame;
  frame.sequence = 7;
  frame.timestamp_ns = 1234;
  frame.width = 1344;
  frame.height = 760;
  frame.is_keyframe = true;
  frame.codec_header = {0x01, 0x02};
  frame.data = {0x10, 0x11, 0x12};

  auto chunks = commaview::video::plan_video_chunks(frame, 16);
  assert(chunks.size() == 1);
  assert(chunks[0].chunk_index == 0);
  assert(chunks[0].chunk_count == 1);
  assert(chunks[0].offset == 0);
  assert((chunks[0].bytes == std::vector<uint8_t>{0x01, 0x02, 0x10, 0x11, 0x12}));
  assert(chunks[0].is_keyframe);
  assert(chunks[0].is_first);
  assert(chunks[0].is_final);
  assert(chunks[0].flags == (commaview::video::VIDEO_CHUNK_KEYFRAME |
                             commaview::video::VIDEO_CHUNK_FIRST |
                             commaview::video::VIDEO_CHUNK_FINAL));
}

void test_multi_chunk_frame_preserves_offsets() {
  commaview::video::VideoFrameForChunking frame;
  frame.sequence = 9;
  frame.data.resize(40);
  auto chunks = commaview::video::plan_video_chunks(frame, 16);
  assert(chunks.size() == 3);
  assert(chunks[0].offset == 0);
  assert(chunks[1].offset == 16);
  assert(chunks[2].offset == 32);
  assert(chunks[2].bytes.size() == 8);
}

void test_chunks_preserve_bytes_across_codec_header_boundary() {
  commaview::video::VideoFrameForChunking frame;
  frame.codec_header = {0x01, 0x02, 0x03};
  frame.data = {0x10, 0x11, 0x12, 0x13};

  auto chunks = commaview::video::plan_video_chunks(frame, 5);
  assert(chunks.size() == 2);
  assert((chunks[0].bytes == std::vector<uint8_t>{0x01, 0x02, 0x03, 0x10, 0x11}));
  assert((chunks[1].bytes == std::vector<uint8_t>{0x12, 0x13}));
  assert(chunks[0].codec_header_len == 3);
  assert(chunks[0].data_len == 4);
  assert(chunks[1].offset == 5);
}

void test_exact_boundary_chunking_has_no_empty_tail_chunk() {
  commaview::video::VideoFrameForChunking frame;
  frame.data = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

  auto chunks = commaview::video::plan_video_chunks(frame, 3);
  assert(chunks.size() == 2);
  assert(chunks[0].chunk_count == 2);
  assert(chunks[1].chunk_index == 1);
  assert(chunks[1].is_final);
  assert(chunks[0].bytes.size() == 3);
  assert(chunks[1].bytes.size() == 3);
  assert((chunks[1].bytes == std::vector<uint8_t>{0x04, 0x05, 0x06}));
}

void test_default_chunk_size_used_when_zero_requested() {
  commaview::video::VideoFrameForChunking frame;
  frame.data.resize(commaview::video::DEFAULT_VIDEO_CHUNK_BYTES + 1);
  frame.data.front() = 0xaa;
  frame.data.back() = 0xbb;

  auto chunks = commaview::video::plan_video_chunks(frame, 0);
  assert(chunks.size() == 2);
  assert(chunks[0].bytes.size() == commaview::video::DEFAULT_VIDEO_CHUNK_BYTES);
  assert(chunks[1].bytes.size() == 1);
  assert(chunks[0].bytes.front() == 0xaa);
  assert(chunks[1].bytes[0] == 0xbb);
}

void test_empty_frames_are_rejected() {
  assert_throws_invalid_argument("empty", []() {
    commaview::video::VideoFrameForChunking frame;
    (void)commaview::video::plan_video_chunks(frame, 16);
  });
}

void test_too_many_chunks_are_rejected() {
  assert_throws_length_error("too many chunks", []() {
    commaview::video::VideoFrameForChunking frame;
    frame.data.resize(static_cast<size_t>(UINT16_MAX) + 1);
    (void)commaview::video::plan_video_chunks(frame, 1);
  });
}

void test_encoded_chunk_matches_golden_wire_payload() {
  commaview::video::VideoChunk chunk;
  chunk.frame_sequence = 0x01020304;
  chunk.chunk_index = 0x0506;
  chunk.chunk_count = 0x0708;
  chunk.flags = 0;  // Encoding derives flags from booleans, not this potentially stale field.
  chunk.is_keyframe = true;
  chunk.is_first = false;
  chunk.is_final = true;
  chunk.timestamp_ns = 0x0102030405060708ULL;
  chunk.width = 0x090a0b0c;
  chunk.height = 0x0d0e0f10;
  chunk.codec_header_len = 0x11121314;
  chunk.data_len = 0x15161718;
  chunk.offset = 0x191a1b1c;
  chunk.bytes = {0xde, 0xad, 0xbe, 0xef};

  const std::vector<uint8_t> expected = {
      0x06,
      0x01, 0x02, 0x03, 0x04,
      0x05, 0x06,
      0x07, 0x08,
      0x05,
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0x09, 0x0a, 0x0b, 0x0c,
      0x0d, 0x0e, 0x0f, 0x10,
      0x11, 0x12, 0x13, 0x14,
      0x15, 0x16, 0x17, 0x18,
      0x19, 0x1a, 0x1b, 0x1c,
      0x00, 0x00, 0x00, 0x04,
      0xde, 0xad, 0xbe, 0xef,
  };

  auto payload = commaview::video::encode_video_chunk_payload(chunk);
  assert(payload == expected);
}

void test_encoded_chunk_round_trips_header_fields() {
  commaview::video::VideoChunk chunk;
  chunk.frame_sequence = 3;
  chunk.chunk_index = 1;
  chunk.chunk_count = 4;
  chunk.is_keyframe = true;
  chunk.timestamp_ns = 99;
  chunk.width = 1928;
  chunk.height = 1208;
  chunk.codec_header_len = 6;
  chunk.data_len = 100;
  chunk.offset = 16;
  chunk.bytes = {0xaa, 0xbb};
  auto payload = commaview::video::encode_video_chunk_payload(chunk);
  assert(payload[0] == commaview::video::MSG_VIDEO_CHUNK);
  auto decoded = commaview::video::decode_video_chunk_payload_for_test(payload);
  assert(decoded.frame_sequence == chunk.frame_sequence);
  assert(decoded.offset == chunk.offset);
  assert(decoded.bytes == chunk.bytes);
  assert(decoded.is_keyframe);
  assert(!decoded.is_first);
  assert(!decoded.is_final);
}

}  // namespace

int main() {
  test_single_chunk_frame();
  test_multi_chunk_frame_preserves_offsets();
  test_chunks_preserve_bytes_across_codec_header_boundary();
  test_exact_boundary_chunking_has_no_empty_tail_chunk();
  test_default_chunk_size_used_when_zero_requested();
  test_empty_frames_are_rejected();
  test_too_many_chunks_are_rejected();
  test_encoded_chunk_matches_golden_wire_payload();
  test_encoded_chunk_round_trips_header_fields();
  return 0;
}
