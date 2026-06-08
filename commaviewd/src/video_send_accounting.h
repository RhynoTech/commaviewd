#pragma once

#include "framing.h"
#include "video_chunk_protocol.h"

#include <algorithm>
#include <cstdint>

namespace commaview::video {

// Applies bridge-runtime counters for one bounded chunk send result.
// A non-OK result after any byte was written corrupts the length-framed TCP stream,
// regardless of whether the immediate status was backpressure or disconnect.
template <typename Counters>
void note_video_chunk_send_counters(Counters& counters,
                                    const VideoChunk& chunk,
                                    const commaview::net::SendResult& result) {
  if (chunk.is_first) counters.frames_chunked += 1;
  counters.max_chunks_per_frame = std::max<uint64_t>(
      counters.max_chunks_per_frame,
      static_cast<uint64_t>(chunk.chunk_count));
  counters.max_chunk_send_micros = std::max(
      counters.max_chunk_send_micros,
      result.elapsed_micros);

  if (result.status == commaview::net::SendStatus::Ok) {
    counters.chunks_sent += 1;
  } else {
    if (result.status == commaview::net::SendStatus::Backpressure && result.bytes_sent == 0) {
      counters.zero_byte_chunk_backpressure_count += 1;
    }
    if (result.bytes_sent > 0) {
      counters.partial_chunk_reset_count += 1;
    }
  }
}

}  // namespace commaview::video
