#pragma once

#include "framing.h"
#include "video_chunk_protocol.h"

#include <cstdint>
#include <string>

namespace commaview::runtime {

struct RuntimeVideoSendStats {
  uint64_t ok_count = 0;
  uint64_t backpressure_count = 0;
  uint64_t disconnect_count = 0;
  uint64_t partial_reset_count = 0;
  uint64_t zero_byte_drop_count = 0;
  uint64_t chunks_sent = 0;
  uint64_t frames_chunked = 0;
  uint64_t frame_abandon_count = 0;
  uint64_t zero_byte_chunk_backpressure_count = 0;
  uint64_t partial_chunk_reset_count = 0;
  uint64_t partial_recovery_attempt_count = 0;
  uint64_t partial_recovery_success_count = 0;
  uint64_t max_chunks_per_frame = 0;
  uint64_t max_chunk_send_micros = 0;
  uint64_t queue_drop_count = 0;
  uint64_t keyframe_wait_drop_count = 0;
  uint64_t queue_high_watermark = 0;
  uint64_t zero_byte_backpressure_recovered_count = 0;
  uint64_t max_queued_frame_age_ms = 0;
  uint64_t max_send_micros = 0;
  std::string last_status = "ok";
  int last_error = 0;
  std::string last_error_name = "none";
  uint64_t last_bytes_sent = 0;
  uint64_t last_elapsed_micros = 0;
  uint64_t last_at_ms = 0;
  bool last_tcp_snapshot_valid = false;
  std::string last_tcp_stream;
  std::string last_tcp_reason;
  uint64_t last_tcp_snapshot_at_ms = 0;
  uint32_t last_tcp_rtt_micros = 0;
  uint32_t last_tcp_total_retrans = 0;
  uint32_t last_tcp_unacked = 0;
  uint32_t last_tcp_cwnd = 0;
  int64_t last_tcp_send_queue_bytes = -1;
  int64_t last_tcp_not_sent_bytes = -1;
};

void note_video_chunk_send_result(RuntimeVideoSendStats& stats,
                                  const commaview::video::VideoChunk& chunk,
                                  const commaview::net::SendResult& result,
                                  uint64_t now_ms);

std::string video_send_stats_json(const RuntimeVideoSendStats& stats);

}  // namespace commaview::runtime
