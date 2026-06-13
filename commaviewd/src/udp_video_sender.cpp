#include "udp_video_sender.h"

#include <netinet/in.h>
#include <set>
#include <utility>

namespace commaview::video {
namespace {

void fill_cache_stats(const UdpVideoRepairCache& cache, UdpVideoSendStats* stats) {
  stats->repair_cache_bytes = cache.total_payload_bytes();
  stats->repair_cache_high_watermark_bytes = cache.high_water_payload_bytes();
}

void fill_cache_stats(const UdpVideoRepairCache& cache, UdpVideoRepairStats* stats) {
  stats->repair_cache_bytes = cache.total_payload_bytes();
  stats->repair_cache_high_watermark_bytes = cache.high_water_payload_bytes();
}

}  // namespace

UdpVideoRepairCache::Limits UdpVideoSender::default_repair_cache_limits() {
  return UdpVideoRepairCache::Limits{
      8U * 1024U * 1024U,
      24U * 1024U * 1024U,
      2LL * 1000LL * 1000LL * 1000LL,
      120,
  };
}

UdpVideoSender::UdpVideoSender(SendFn send_fn,
                               UdpVideoRepairCache::Limits repair_cache_limits,
                               size_t target_payload_bytes,
                               int64_t client_timeout_ns)
    : send_fn_(std::move(send_fn)),
      repair_cache_(repair_cache_limits),
      target_payload_bytes_(target_payload_bytes),
      client_timeout_ns_(client_timeout_ns) {}

void UdpVideoSender::note_client_hello(UdpVideoStreamId stream,
                                       sockaddr_storage addr,
                                       socklen_t addr_len,
                                       uint16_t session_id,
                                       int64_t now_ns) {
  checkin_endpoint(stream, addr, addr_len, session_id, now_ns);
}

void UdpVideoSender::note_client_policy(UdpVideoStreamId stream,
                                        sockaddr_storage addr,
                                        socklen_t addr_len,
                                        uint16_t session_id,
                                        bool suppress_video,
                                        int64_t now_ns) {
  Endpoint* endpoint = checkin_endpoint(stream, addr, addr_len, session_id, now_ns);
  if (endpoint != nullptr) {
    endpoint->suppress_video = suppress_video;
  }
}

bool UdpVideoSender::has_active_client(UdpVideoStreamId stream, int64_t now_ns) const {
  const Endpoint* endpoint = endpoint_for_stream(stream);
  return endpoint != nullptr && !endpoint_expired(*endpoint, now_ns);
}

bool UdpVideoSender::client_suppresses_video(UdpVideoStreamId stream) const {
  const Endpoint* endpoint = endpoint_for_stream(stream);
  return endpoint != nullptr && endpoint->suppress_video;
}

bool UdpVideoSender::active_client_session(UdpVideoStreamId stream,
                                           int64_t now_ns,
                                           uint16_t* session_id) const {
  const Endpoint* endpoint = endpoint_for_stream(stream);
  if (endpoint == nullptr || endpoint_expired(*endpoint, now_ns)) return false;
  if (session_id != nullptr) *session_id = endpoint->session_id;
  return true;
}

size_t UdpVideoSender::send_raw_datagrams(UdpVideoStreamId stream,
                                          const std::vector<std::vector<uint8_t>>& datagrams,
                                          int64_t now_ns) {
  Endpoint* endpoint = active_endpoint_for_stream(stream, now_ns);
  if (endpoint == nullptr) return 0;
  size_t sent = 0;
  for (const std::vector<uint8_t>& datagram : datagrams) {
    const ssize_t result =
        send_fn_(datagram.data(), datagram.size(), endpoint->addr, endpoint->addr_len);
    if (result < 0 || static_cast<size_t>(result) != datagram.size()) break;
    sent++;
  }
  return sent;
}

UdpVideoSendStats UdpVideoSender::send_frame(const UdpVideoFrameForPacketizing& frame,
                                             int64_t now_ns) {
  UdpVideoSendStats stats;
  stats.frames_attempted = 1;

  const Endpoint* endpoint = active_endpoint_for_stream(frame.stream_id, now_ns);
  if (endpoint == nullptr) {
    stats.dropped_no_client = 1;
    fill_cache_stats(repair_cache_, &stats);
    return stats;
  }
  if (endpoint->suppress_video) {
    stats.dropped_suppressed = 1;
    fill_cache_stats(repair_cache_, &stats);
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

  fill_cache_stats(repair_cache_, &stats);
  return stats;
}

UdpVideoRepairStats UdpVideoSender::handle_repair_request(const UdpVideoRepairRequest& request,
                                                          int64_t now_ns) {
  UdpVideoRepairStats stats;
  stats.requests = 1;
  repair_cache_.evict_expired(now_ns);

  const Endpoint* endpoint = active_endpoint_for_stream(request.stream_id, now_ns);
  if (endpoint == nullptr || endpoint->session_id != request.session_id) {
    stats.missing_count = request.packet_indexes.empty() ? 1 : std::set<uint16_t>(
        request.packet_indexes.begin(), request.packet_indexes.end()).size();
    fill_cache_stats(repair_cache_, &stats);
    return stats;
  }

  std::vector<UdpVideoPacket> packets;
  size_t requested_count = 1;
  if (request.packet_indexes.empty()) {
    packets = repair_cache_.lookup_frame(request.stream_id, request.session_id, request.frame_sequence);
  } else {
    std::vector<uint16_t> unique_indexes;
    std::set<uint16_t> seen_indexes;
    for (const uint16_t packet_index : request.packet_indexes) {
      if (seen_indexes.insert(packet_index).second) {
        unique_indexes.push_back(packet_index);
      }
    }
    requested_count = unique_indexes.size();
    packets = repair_cache_.lookup(request.stream_id,
                                   request.session_id,
                                   request.frame_sequence,
                                   unique_indexes);
  }

  if (packets.size() < requested_count) {
    stats.missing_count = requested_count - packets.size();
  }

  if (packets.empty()) {
    fill_cache_stats(repair_cache_, &stats);
    return stats;
  }

  for (const auto& packet : packets) {
    if (send_packet(packet, *endpoint)) {
      stats.packets_resent += 1;
    } else {
      stats.send_errors += 1;
    }
  }

  fill_cache_stats(repair_cache_, &stats);
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

UdpVideoSender::Endpoint* UdpVideoSender::checkin_endpoint(UdpVideoStreamId stream,
                                                           const sockaddr_storage& addr,
                                                           socklen_t addr_len,
                                                           uint16_t session_id,
                                                           int64_t now_ns) {
  if (!is_valid_endpoint(addr, addr_len)) {
    return nullptr;
  }

  auto it = endpoints_.find(stream);
  if (it != endpoints_.end() && it->second.session_id == session_id) {
    it->second.addr = addr;
    it->second.addr_len = addr_len;
    it->second.last_seen_ns = now_ns;
    return &it->second;
  }

  Endpoint endpoint;
  endpoint.addr = addr;
  endpoint.addr_len = addr_len;
  endpoint.session_id = session_id;
  endpoint.last_seen_ns = now_ns;
  return &(endpoints_[stream] = endpoint);
}

UdpVideoSender::Endpoint* UdpVideoSender::active_endpoint_for_stream(UdpVideoStreamId stream,
                                                                     int64_t now_ns) {
  auto it = endpoints_.find(stream);
  if (it == endpoints_.end()) {
    return nullptr;
  }
  if (endpoint_expired(it->second, now_ns)) {
    endpoints_.erase(it);
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

bool UdpVideoSender::endpoint_expired(const Endpoint& endpoint, int64_t now_ns) const {
  if (client_timeout_ns_ <= 0) {
    return false;
  }
  return now_ns >= endpoint.last_seen_ns && now_ns - endpoint.last_seen_ns > client_timeout_ns_;
}

bool UdpVideoSender::is_valid_endpoint(const sockaddr_storage& addr, socklen_t addr_len) const {
  if (addr_len == 0 || addr_len > sizeof(sockaddr_storage) || addr_len < sizeof(sa_family_t)) {
    return false;
  }

  switch (addr.ss_family) {
    case AF_INET:
      return addr_len == sizeof(sockaddr_in);
    case AF_INET6:
      return addr_len == sizeof(sockaddr_in6);
    default:
      return false;
  }
}

}  // namespace commaview::video
