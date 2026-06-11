#pragma once

#include "udp_video_packetizer.h"
#include "udp_video_repair_cache.h"
#include "udp_video_protocol.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <sys/socket.h>
#include <sys/types.h>
#include <vector>

namespace commaview::video {

struct UdpVideoRepairRequest {
  UdpVideoStreamId stream_id = UdpVideoStreamId::Road;
  uint16_t session_id = 0;
  uint32_t frame_sequence = 0;
  std::vector<uint16_t> packet_indexes;
};

struct UdpVideoSendStats {
  uint64_t frames_attempted = 0;
  uint64_t dropped_no_client = 0;
  uint64_t packets_packetized = 0;
  uint64_t packets_sent = 0;
  uint64_t send_errors = 0;
  uint64_t repair_cache_bytes = 0;
  uint64_t repair_cache_high_watermark_bytes = 0;
  bool peer_reset_required = false;
};

struct UdpVideoRepairStats {
  uint64_t requests = 0;
  uint64_t packets_resent = 0;
  uint64_t missing_count = 0;
  uint64_t send_errors = 0;
  uint64_t repair_cache_bytes = 0;
  uint64_t repair_cache_high_watermark_bytes = 0;
  bool peer_reset_required = false;
};

class UdpVideoSender {
 public:
  using SendFn = std::function<ssize_t(const uint8_t*, size_t, const sockaddr_storage&, socklen_t)>;

  explicit UdpVideoSender(
      SendFn send_fn,
      UdpVideoRepairCache::Limits repair_cache_limits = default_repair_cache_limits(),
      size_t target_payload_bytes = UDP_VIDEO_TARGET_PAYLOAD_BYTES);

  static UdpVideoRepairCache::Limits default_repair_cache_limits();

  void note_client_hello(UdpVideoStreamId stream,
                         sockaddr_storage addr,
                         socklen_t addr_len,
                         uint16_t session_id);

  UdpVideoSendStats send_frame(const UdpVideoFrameForPacketizing& frame, int64_t now_ns);
  UdpVideoRepairStats handle_repair_request(const UdpVideoRepairRequest& request, int64_t now_ns);

 private:
  struct Endpoint {
    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    uint16_t session_id = 0;
  };

  bool send_packet(const UdpVideoPacket& packet, const Endpoint& endpoint);
  Endpoint* endpoint_for_stream(UdpVideoStreamId stream);
  const Endpoint* endpoint_for_stream(UdpVideoStreamId stream) const;
  bool is_valid_endpoint(const sockaddr_storage& addr, socklen_t addr_len) const;

  SendFn send_fn_;
  UdpVideoRepairCache repair_cache_;
  size_t target_payload_bytes_ = UDP_VIDEO_TARGET_PAYLOAD_BYTES;
  uint64_t next_packet_sequence_ = 1;
  std::map<UdpVideoStreamId, Endpoint> endpoints_;
};

}  // namespace commaview::video
