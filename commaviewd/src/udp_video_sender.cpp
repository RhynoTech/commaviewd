#include "udp_video_sender.h"

#include <utility>

namespace commaview::video {

UdpVideoRepairCache::Limits UdpVideoSender::default_repair_cache_limits() {
  return UdpVideoRepairCache::Limits{
      8U * 1024U * 1024U,
      24U * 1024U * 1024U,
      2LL * 1000LL * 1000LL * 1000LL,
  };
}

UdpVideoSender::UdpVideoSender(SendFn send_fn,
                               UdpVideoRepairCache::Limits repair_cache_limits,
                               size_t target_payload_bytes)
    : send_fn_(std::move(send_fn)),
      repair_cache_(repair_cache_limits),
      target_payload_bytes_(target_payload_bytes) {}

void UdpVideoSender::note_client_hello(UdpVideoStreamId stream,
                                       sockaddr_storage addr,
                                       socklen_t addr_len,
                                       uint16_t session_id) {
  Endpoint endpoint;
  endpoint.addr = addr;
  endpoint.addr_len = addr_len;
  endpoint.session_id = session_id;
  endpoints_[stream] = endpoint;
}

UdpVideoSendStats UdpVideoSender::send_frame(const UdpVideoFrameForPacketizing& frame,
                                             int64_t now_ns) {
  UdpVideoSendStats stats;
  stats.frames_attempted = 1;

  const Endpoint* endpoint = endpoint_for_stream(frame.stream_id);
  if (endpoint == nullptr) {
    stats.dropped_no_client = 1;
    return stats;
  }

  UdpVideoFrameForPacketizing frame_for_send = frame;
  frame_for_send.session_id = endpoint->session_id;
  auto packets = packetize_udp_video_frame(frame_for_send, &next_packet_sequence_, target_payload_bytes_);
  stats.packets_packetized = packets.size();

  repair_cache_.store(packets, now_ns);

  for (const auto& packet : packets) {
    if (send_packet(packet, *endpoint)) {
      stats.packets_sent += 1;
    } else {
      stats.send_errors += 1;
    }
  }

  return stats;
}

UdpVideoRepairStats UdpVideoSender::handle_repair_request(const UdpVideoRepairRequest& request,
                                                          int64_t now_ns) {
  UdpVideoRepairStats stats;
  stats.requests = 1;
  repair_cache_.evict_expired(now_ns);

  const Endpoint* endpoint = endpoint_for_stream(request.stream_id);
  if (endpoint == nullptr || endpoint->session_id != request.session_id) {
    stats.missing_count = 1;
    return stats;
  }

  std::vector<UdpVideoPacket> packets;
  if (request.packet_indexes.empty()) {
    packets = repair_cache_.lookup_frame(request.stream_id, request.session_id, request.frame_sequence);
  } else {
    packets = repair_cache_.lookup(request.stream_id,
                                   request.session_id,
                                   request.frame_sequence,
                                   request.packet_indexes);
  }

  if (packets.empty()) {
    stats.missing_count = 1;
    return stats;
  }

  for (const auto& packet : packets) {
    if (send_packet(packet, *endpoint)) {
      stats.packets_resent += 1;
    } else {
      stats.send_errors += 1;
    }
  }

  return stats;
}

bool UdpVideoSender::send_packet(const UdpVideoPacket& packet, const Endpoint& endpoint) {
  if (!send_fn_) {
    return false;
  }
  const std::vector<uint8_t> bytes = encode_udp_video_packet(packet);
  const ssize_t sent = send_fn_(bytes.data(), bytes.size(), endpoint.addr, endpoint.addr_len);
  return sent == static_cast<ssize_t>(bytes.size());
}

UdpVideoSender::Endpoint* UdpVideoSender::endpoint_for_stream(UdpVideoStreamId stream) {
  auto it = endpoints_.find(stream);
  if (it == endpoints_.end()) {
    return nullptr;
  }
  return &it->second;
}

const UdpVideoSender::Endpoint* UdpVideoSender::endpoint_for_stream(UdpVideoStreamId stream) const {
  auto it = endpoints_.find(stream);
  if (it == endpoints_.end()) {
    return nullptr;
  }
  return &it->second;
}

}  // namespace commaview::video
