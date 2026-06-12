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

// A client that has not sent Hello/Heartbeat/Policy within this window is
// treated as gone and video sending stops until it checks in again.
constexpr int64_t UDP_VIDEO_CLIENT_TIMEOUT_NS = 3LL * 1000LL * 1000LL * 1000LL;

struct UdpVideoRepairRequest {
  UdpVideoStreamId stream_id = UdpVideoStreamId::Road;
  uint16_t session_id = 0;
  uint32_t frame_sequence = 0;
  std::vector<uint16_t> packet_indexes;
};

struct UdpVideoSendStats {
  uint64_t frames_attempted = 0;
  uint64_t dropped_no_client = 0;
  uint64_t dropped_suppressed = 0;
  uint64_t packets_packetized = 0;
  uint64_t packets_sent = 0;
  uint64_t send_errors = 0;
  uint64_t repair_cache_bytes = 0;
  uint64_t repair_cache_high_watermark_bytes = 0;
};

struct UdpVideoRepairStats {
  uint64_t requests = 0;
  uint64_t packets_resent = 0;
  uint64_t missing_count = 0;
  uint64_t send_errors = 0;
  uint64_t repair_cache_bytes = 0;
  uint64_t repair_cache_high_watermark_bytes = 0;
};

class UdpVideoSender {
 public:
  using SendFn = std::function<ssize_t(const uint8_t*, size_t, const sockaddr_storage&, socklen_t)>;

  explicit UdpVideoSender(
      SendFn send_fn,
      UdpVideoRepairCache::Limits repair_cache_limits = default_repair_cache_limits(),
      size_t target_payload_bytes = UDP_VIDEO_TARGET_PAYLOAD_BYTES,
      int64_t client_timeout_ns = UDP_VIDEO_CLIENT_TIMEOUT_NS);

  static UdpVideoRepairCache::Limits default_repair_cache_limits();

  // Hello refreshes client liveness; a new session id resets the suppress flag.
  void note_client_hello(UdpVideoStreamId stream,
                         sockaddr_storage addr,
                         socklen_t addr_len,
                         uint16_t session_id,
                         int64_t now_ns = 0);

  // Heartbeat/Policy datagrams refresh liveness and carry the suppress flag.
  void note_client_policy(UdpVideoStreamId stream,
                          sockaddr_storage addr,
                          socklen_t addr_len,
                          uint16_t session_id,
                          bool suppress_video,
                          int64_t now_ns = 0);

  bool has_active_client(UdpVideoStreamId stream, int64_t now_ns) const;
  bool client_suppresses_video(UdpVideoStreamId stream) const;

  // Returns true and the active client's session id when a client is within
  // the liveness window for the stream.
  bool active_client_session(UdpVideoStreamId stream, int64_t now_ns, uint16_t* session_id) const;

  // Sends pre-encoded datagrams to the stream's active client. Stops at the
  // first send failure (latest-wins data is dropped, never retried) and
  // returns the number of datagrams handed to the socket.
  size_t send_raw_datagrams(UdpVideoStreamId stream,
                            const std::vector<std::vector<uint8_t>>& datagrams,
                            int64_t now_ns);

  UdpVideoSendStats send_frame(const UdpVideoFrameForPacketizing& frame, int64_t now_ns);
  UdpVideoRepairStats handle_repair_request(const UdpVideoRepairRequest& request, int64_t now_ns);

 private:
  struct Endpoint {
    sockaddr_storage addr{};
    socklen_t addr_len = 0;
    uint16_t session_id = 0;
    int64_t last_seen_ns = 0;
    bool suppress_video = false;
  };

  bool send_packet(const UdpVideoPacket& packet, const Endpoint& endpoint);
  Endpoint* checkin_endpoint(UdpVideoStreamId stream,
                             const sockaddr_storage& addr,
                             socklen_t addr_len,
                             uint16_t session_id,
                             int64_t now_ns);
  Endpoint* active_endpoint_for_stream(UdpVideoStreamId stream, int64_t now_ns);
  const Endpoint* endpoint_for_stream(UdpVideoStreamId stream) const;
  bool endpoint_expired(const Endpoint& endpoint, int64_t now_ns) const;
  bool is_valid_endpoint(const sockaddr_storage& addr, socklen_t addr_len) const;

  SendFn send_fn_;
  UdpVideoRepairCache repair_cache_;
  size_t target_payload_bytes_ = UDP_VIDEO_TARGET_PAYLOAD_BYTES;
  int64_t client_timeout_ns_ = UDP_VIDEO_CLIENT_TIMEOUT_NS;
  uint64_t next_packet_sequence_ = 1;
  std::map<UdpVideoStreamId, Endpoint> endpoints_;
};

}  // namespace commaview::video
