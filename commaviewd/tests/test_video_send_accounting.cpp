#include "video_send_accounting.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace {

struct Counters {
  uint64_t chunks_sent = 0;
  uint64_t frames_chunked = 0;
  uint64_t zero_byte_chunk_backpressure_count = 0;
  uint64_t partial_chunk_reset_count = 0;
  uint64_t max_chunks_per_frame = 0;
  uint64_t max_chunk_send_micros = 0;
};

commaview::video::VideoChunk chunk(bool first = false) {
  commaview::video::VideoChunk chunk;
  chunk.frame_sequence = 7;
  chunk.chunk_index = first ? 0 : 1;
  chunk.chunk_count = 3;
  chunk.is_first = first;
  return chunk;
}

commaview::net::SendResult result(commaview::net::SendStatus status, size_t bytes_sent) {
  commaview::net::SendResult result;
  result.status = status;
  result.bytes_sent = bytes_sent;
  result.elapsed_micros = 123;
  return result;
}

void test_partial_backpressure_increments_partial_reset() {
  Counters counters;
  commaview::video::note_video_chunk_send_counters(
      counters,
      chunk(),
      result(commaview::net::SendStatus::Backpressure, 12));

  assert(counters.partial_chunk_reset_count == 1);
  assert(counters.zero_byte_chunk_backpressure_count == 0);
  assert(counters.chunks_sent == 0);
}

void test_partial_disconnect_increments_partial_reset() {
  Counters counters;
  commaview::video::note_video_chunk_send_counters(
      counters,
      chunk(),
      result(commaview::net::SendStatus::Disconnected, 12));

  assert(counters.partial_chunk_reset_count == 1);
  assert(counters.zero_byte_chunk_backpressure_count == 0);
  assert(counters.chunks_sent == 0);
}

void test_partial_invalid_argument_increments_partial_reset() {
  Counters counters;
  commaview::video::note_video_chunk_send_counters(
      counters,
      chunk(),
      result(commaview::net::SendStatus::InvalidArgument, 12));

  assert(counters.partial_chunk_reset_count == 1);
  assert(counters.zero_byte_chunk_backpressure_count == 0);
  assert(counters.chunks_sent == 0);
}

void test_zero_byte_backpressure_counts_abandon_path_only() {
  Counters counters;
  commaview::video::note_video_chunk_send_counters(
      counters,
      chunk(),
      result(commaview::net::SendStatus::Backpressure, 0));

  assert(counters.zero_byte_chunk_backpressure_count == 1);
  assert(counters.partial_chunk_reset_count == 0);
  assert(counters.chunks_sent == 0);
}

void test_ok_counts_sent_chunk_without_reset() {
  Counters counters;
  commaview::video::note_video_chunk_send_counters(
      counters,
      chunk(true),
      result(commaview::net::SendStatus::Ok, 12));

  assert(counters.chunks_sent == 1);
  assert(counters.frames_chunked == 1);
  assert(counters.max_chunks_per_frame == 3);
  assert(counters.max_chunk_send_micros == 123);
  assert(counters.zero_byte_chunk_backpressure_count == 0);
  assert(counters.partial_chunk_reset_count == 0);
}

}  // namespace

int main() {
  test_partial_backpressure_increments_partial_reset();
  test_partial_disconnect_increments_partial_reset();
  test_partial_invalid_argument_increments_partial_reset();
  test_zero_byte_backpressure_counts_abandon_path_only();
  test_ok_counts_sent_chunk_without_reset();
  return 0;
}
