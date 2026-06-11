#pragma once

#include "udp_video_protocol.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace commaview::video {

class UdpVideoRepairCache {
 public:
  struct Limits {
    size_t max_bytes_per_stream = 0;
    size_t max_bytes_total = 0;
    int64_t max_age_ns = 0;
  };

  explicit UdpVideoRepairCache(Limits limits);

  void store(const std::vector<UdpVideoPacket>& packets, int64_t now_ns);
  std::vector<UdpVideoPacket> lookup(UdpVideoStreamId stream,
                                     uint16_t session_id,
                                     uint32_t frame_sequence,
                                     const std::vector<uint16_t>& packet_indexes) const;
  std::vector<UdpVideoPacket> lookup_frame(UdpVideoStreamId stream,
                                           uint16_t session_id,
                                           uint32_t frame_sequence) const;
  void evict_expired(int64_t now_ns);

 private:
  struct FrameKey {
    UdpVideoStreamId stream = UdpVideoStreamId::Road;
    uint16_t session_id = 0;
    uint32_t frame_sequence = 0;

    bool operator<(const FrameKey& other) const;
  };

  struct CachedFrame {
    std::vector<UdpVideoPacket> packets;
    size_t payload_bytes = 0;
    int64_t stored_ns = 0;
    uint64_t insertion_order = 0;
    bool keyframe_or_csd = false;
  };

  using FrameMap = std::map<FrameKey, CachedFrame>;

  static bool build_cached_frame(const std::vector<UdpVideoPacket>& packets,
                                 int64_t now_ns,
                                 uint64_t insertion_order,
                                 CachedFrame* out);

  void remove_frame(FrameMap::iterator it);
  void enforce_caps();
  void enforce_stream_cap(UdpVideoStreamId stream);
  FrameMap::iterator choose_eviction_candidate_for_stream(UdpVideoStreamId stream);
  FrameMap::iterator choose_eviction_candidate_global();
  FrameMap::const_iterator find_latest_keyframe(UdpVideoStreamId stream) const;
  size_t stream_bytes(UdpVideoStreamId stream) const;
  void add_stream_bytes(UdpVideoStreamId stream, size_t bytes);
  void subtract_stream_bytes(UdpVideoStreamId stream, size_t bytes);

  Limits limits_;
  FrameMap frames_;
  std::map<UdpVideoStreamId, size_t> stream_payload_bytes_;
  size_t total_payload_bytes_ = 0;
  uint64_t next_insertion_order_ = 1;
};

}  // namespace commaview::video
