#include "runtime_video_send_accounting.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

namespace {

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

void test_partial_backpressure_increments_bridge_partial_reset() {
  commaview::runtime::RuntimeVideoSendStats stats;
  commaview::runtime::note_video_chunk_send_result(
      stats,
      chunk(),
      result(commaview::net::SendStatus::Backpressure, 12),
      1000);

  assert(stats.backpressure_count == 1);
  assert(stats.partial_reset_count == 1);
  assert(stats.partial_chunk_reset_count == 1);
  assert(stats.zero_byte_drop_count == 0);
  assert(stats.zero_byte_chunk_backpressure_count == 0);
  assert(stats.chunks_sent == 0);
  assert(stats.last_status == "backpressure");
  assert(stats.last_bytes_sent == 12);
  assert(stats.last_at_ms == 1000);
}

void test_partial_disconnect_increments_bridge_partial_chunk_reset() {
  commaview::runtime::RuntimeVideoSendStats stats;
  commaview::runtime::note_video_chunk_send_result(
      stats,
      chunk(),
      result(commaview::net::SendStatus::Disconnected, 12),
      1001);

  assert(stats.disconnect_count == 1);
  assert(stats.partial_reset_count == 0);
  assert(stats.partial_chunk_reset_count == 1);
  assert(stats.zero_byte_chunk_backpressure_count == 0);
  assert(stats.chunks_sent == 0);
  assert(stats.last_status == "disconnected");
  assert(stats.last_bytes_sent == 12);
}

void test_partial_invalid_argument_increments_bridge_partial_chunk_reset() {
  commaview::runtime::RuntimeVideoSendStats stats;
  commaview::runtime::note_video_chunk_send_result(
      stats,
      chunk(),
      result(commaview::net::SendStatus::InvalidArgument, 12),
      1002);

  assert(stats.disconnect_count == 1);
  assert(stats.partial_chunk_reset_count == 1);
  assert(stats.zero_byte_chunk_backpressure_count == 0);
  assert(stats.chunks_sent == 0);
  assert(stats.last_status == "invalid_argument");
}

void test_zero_byte_backpressure_counts_bridge_abandon_path_only() {
  commaview::runtime::RuntimeVideoSendStats stats;
  commaview::runtime::note_video_chunk_send_result(
      stats,
      chunk(),
      result(commaview::net::SendStatus::Backpressure, 0),
      1003);

  assert(stats.backpressure_count == 1);
  assert(stats.zero_byte_drop_count == 1);
  assert(stats.zero_byte_chunk_backpressure_count == 1);
  assert(stats.partial_reset_count == 0);
  assert(stats.partial_chunk_reset_count == 0);
  assert(stats.chunks_sent == 0);
}

void test_ok_counts_bridge_chunk_counters_only() {
  commaview::runtime::RuntimeVideoSendStats stats;
  commaview::runtime::note_video_chunk_send_result(
      stats,
      chunk(true),
      result(commaview::net::SendStatus::Ok, 12),
      1004);

  assert(stats.ok_count == 1);
  assert(stats.chunks_sent == 1);
  assert(stats.frames_chunked == 1);
  assert(stats.max_chunks_per_frame == 3);
  assert(stats.max_chunk_send_micros == 123);
  assert(stats.max_send_micros == 123);
  assert(stats.backpressure_count == 0);
  assert(stats.disconnect_count == 0);
  assert(stats.zero_byte_chunk_backpressure_count == 0);
  assert(stats.partial_chunk_reset_count == 0);
  assert(stats.last_status == "ok");
}

void test_serialized_video_send_json_includes_chunk_and_udp_accounting() {
  commaview::runtime::RuntimeVideoSendStats stats;
  commaview::runtime::note_video_chunk_send_result(
      stats,
      chunk(true),
      result(commaview::net::SendStatus::Ok, 12),
      1005);
  commaview::runtime::note_video_chunk_send_result(
      stats,
      chunk(),
      result(commaview::net::SendStatus::Backpressure, 0),
      1006);
  stats.frame_abandon_count = 1;
  stats.udp_packets_sent = 7;
  stats.udp_send_error_count = 2;
  stats.udp_repair_requests = 3;
  stats.udp_repair_packets_resent = 4;
  stats.udp_repair_miss_count = 5;
  stats.udp_repair_cache_bytes = 600;
  stats.udp_repair_cache_high_watermark_bytes = 700;

  const std::string json = commaview::runtime::video_send_stats_json(stats);
  assert(json.find("\"okCount\":1") != std::string::npos);
  assert(json.find("\"chunksSent\":1") != std::string::npos);
  assert(json.find("\"framesChunked\":1") != std::string::npos);
  assert(json.find("\"frameAbandonCount\":1") != std::string::npos);
  assert(json.find("\"zeroByteChunkBackpressureCount\":1") != std::string::npos);
  assert(json.find("\"partialChunkResetCount\":0") != std::string::npos);
  assert(json.find("\"udpPacketsSent\":7") != std::string::npos);
  assert(json.find("\"udpSendErrorCount\":2") != std::string::npos);
  assert(json.find("\"udpRepairRequests\":3") != std::string::npos);
  assert(json.find("\"udpRepairPacketsResent\":4") != std::string::npos);
  assert(json.find("\"udpRepairMissCount\":5") != std::string::npos);
  assert(json.find("\"udpRepairRequested\":3") != std::string::npos);
  assert(json.find("\"udpRepairRecovered\":4") != std::string::npos);
  assert(json.find("\"udpRepairExpired\":5") != std::string::npos);
  assert(json.find("\"udpRepairCacheBytes\":600") != std::string::npos);
  assert(json.find("\"udpRepairCacheHighWatermarkBytes\":700") != std::string::npos);
  assert(json.find("\"lastStatus\":\"backpressure\"") != std::string::npos);
}

}  // namespace

int main() {
  test_partial_backpressure_increments_bridge_partial_reset();
  test_partial_disconnect_increments_bridge_partial_chunk_reset();
  test_partial_invalid_argument_increments_bridge_partial_chunk_reset();
  test_zero_byte_backpressure_counts_bridge_abandon_path_only();
  test_ok_counts_bridge_chunk_counters_only();
  test_serialized_video_send_json_includes_chunk_and_udp_accounting();
  return 0;
}
