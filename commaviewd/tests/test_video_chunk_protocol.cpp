#include "video_chunk_protocol.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

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
  assert(chunks[0].bytes.size() == 5);
  assert(chunks[0].is_keyframe);
  assert(chunks[0].is_first);
  assert(chunks[0].is_final);
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

void test_encoded_chunk_round_trips_header_fields() {
  commaview::video::VideoChunk chunk;
  chunk.frame_sequence = 3;
  chunk.chunk_index = 1;
  chunk.chunk_count = 4;
  chunk.flags = commaview::video::VIDEO_CHUNK_KEYFRAME;
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
}

}  // namespace

int main() {
  test_single_chunk_frame();
  test_multi_chunk_frame_preserves_offsets();
  test_encoded_chunk_round_trips_header_fields();
  return 0;
}
