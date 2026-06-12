#include "runtime_video_send_accounting.h"

#include "runtime_debug_config.h"

#include <algorithm>
#include <sstream>

namespace commaview::runtime {
namespace {

void note_video_send_failure_details(RuntimeVideoSendStats& stats,
                                     const commaview::net::SendResult& result,
                                     uint64_t now_ms) {
  stats.last_status = commaview::net::send_status_name(result.status);
  stats.last_error = result.error;
  stats.last_error_name = commaview::net::send_error_name(result.error);
  stats.last_bytes_sent = static_cast<uint64_t>(result.bytes_sent);
  stats.last_elapsed_micros = result.elapsed_micros;
  stats.last_at_ms = now_ms;
}

}  // namespace

void note_video_send_result(RuntimeVideoSendStats& stats,
                            const commaview::net::SendResult& result,
                            uint64_t now_ms) {
  stats.max_send_micros = std::max(stats.max_send_micros, result.elapsed_micros);
  if (result.status != commaview::net::SendStatus::Ok) {
    note_video_send_failure_details(stats, result, now_ms);
  }
  switch (result.status) {
    case commaview::net::SendStatus::Ok:
      stats.ok_count += 1;
      break;
    case commaview::net::SendStatus::Backpressure:
      stats.backpressure_count += 1;
      if (result.bytes_sent == 0) stats.zero_byte_drop_count += 1;
      if (result.bytes_sent > 0) stats.partial_reset_count += 1;
      break;
    case commaview::net::SendStatus::Disconnected:
    case commaview::net::SendStatus::InvalidArgument:
      stats.disconnect_count += 1;
      break;
  }
}

std::string video_send_stats_json(const RuntimeVideoSendStats& stats) {
  std::ostringstream out;
  out << "{";
  out << "\"okCount\":" << stats.ok_count << ",";
  out << "\"backpressureCount\":" << stats.backpressure_count << ",";
  out << "\"disconnectCount\":" << stats.disconnect_count << ",";
  out << "\"partialResetCount\":" << stats.partial_reset_count << ",";
  out << "\"zeroByteDropCount\":" << stats.zero_byte_drop_count << ",";
  out << "\"frameAbandonCount\":" << stats.frame_abandon_count << ",";
  out << "\"queueDropCount\":" << stats.queue_drop_count << ",";
  out << "\"keyframeWaitDropCount\":" << stats.keyframe_wait_drop_count << ",";
  out << "\"queueHighWatermark\":" << stats.queue_high_watermark << ",";
  out << "\"zeroByteBackpressureRecoveredCount\":" << stats.zero_byte_backpressure_recovered_count << ",";
  out << "\"maxQueuedFrameAgeMs\":" << stats.max_queued_frame_age_ms << ",";
  out << "\"maxSendMicros\":" << stats.max_send_micros << ",";
  out << "\"udpPacketsSent\":" << stats.udp_packets_sent << ",";
  out << "\"udpSendErrorCount\":" << stats.udp_send_error_count << ",";
  out << "\"udpNoClientDropCount\":" << stats.udp_no_client_drop_count << ",";
  out << "\"udpSuppressedDropCount\":" << stats.udp_suppressed_drop_count << ",";
  out << "\"udpRepairRequests\":" << stats.udp_repair_requests << ",";
  out << "\"udpRepairPacketsResent\":" << stats.udp_repair_packets_resent << ",";
  out << "\"udpRepairMissCount\":" << stats.udp_repair_miss_count << ",";
  out << "\"udpRepairRequested\":" << stats.udp_repair_requests << ",";
  out << "\"udpRepairRecovered\":" << stats.udp_repair_packets_resent << ",";
  out << "\"udpRepairExpired\":" << stats.udp_repair_miss_count << ",";
  out << "\"udpRepairCacheBytes\":" << stats.udp_repair_cache_bytes << ",";
  out << "\"udpRepairCacheHighWatermarkBytes\":" << stats.udp_repair_cache_high_watermark_bytes << ",";
  out << "\"lastStatus\":\"" << commaview::runtime_debug::json_escape(stats.last_status) << "\",";
  out << "\"lastError\":" << stats.last_error << ",";
  out << "\"lastErrorName\":\"" << commaview::runtime_debug::json_escape(stats.last_error_name) << "\",";
  out << "\"lastBytesSent\":" << stats.last_bytes_sent << ",";
  out << "\"lastElapsedMicros\":" << stats.last_elapsed_micros << ",";
  out << "\"lastAtMs\":" << stats.last_at_ms;
  out << "}";
  return out.str();
}

}  // namespace commaview::runtime
