#include "udp_video_repair_cache.h"

#include <algorithm>
#include <cstdint>

namespace commaview::video {
namespace {

uint8_t stream_sort_value(UdpVideoStreamId stream) {
  return static_cast<uint8_t>(stream);
}

bool packet_has_keyframe_or_csd(const UdpVideoPacket& packet) {
  return (packet.flags & (UDP_VIDEO_FLAG_KEYFRAME | UDP_VIDEO_FLAG_CSD_PRESENT)) != 0;
}

void mark_repair_resend(UdpVideoPacket* packet) {
  packet->flags |= UDP_VIDEO_FLAG_REPAIR_RESEND;
}

}  // namespace

bool UdpVideoRepairCache::FrameKey::operator<(const FrameKey& other) const {
  if (stream_sort_value(stream) != stream_sort_value(other.stream)) {
    return stream_sort_value(stream) < stream_sort_value(other.stream);
  }
  return frame_sequence < other.frame_sequence;
}

UdpVideoRepairCache::UdpVideoRepairCache(Limits limits) : limits_(limits) {}

void UdpVideoRepairCache::store(const std::vector<UdpVideoPacket>& packets, int64_t now_ns) {
  CachedFrame cached;
  if (!build_cached_frame(packets, now_ns, next_insertion_order_, &cached)) {
    return;
  }

  const FrameKey key{cached.packets.front().stream_id, cached.packets.front().frame_sequence};
  const auto existing = frames_.find(key);
  if (existing != frames_.end()) {
    remove_frame(existing);
  }

  ++next_insertion_order_;
  add_stream_bytes(key.stream, cached.payload_bytes);
  total_payload_bytes_ += cached.payload_bytes;
  frames_.emplace(key, std::move(cached));
  evict_expired(now_ns);
  enforce_caps();
}

std::vector<UdpVideoPacket> UdpVideoRepairCache::lookup(
    UdpVideoStreamId stream,
    uint32_t frame_sequence,
    const std::vector<uint16_t>& packet_indexes) const {
  const auto it = frames_.find(FrameKey{stream, frame_sequence});
  if (it == frames_.end()) {
    return {};
  }

  std::vector<UdpVideoPacket> out;
  out.reserve(packet_indexes.size());
  for (const uint16_t packet_index : packet_indexes) {
    if (packet_index >= it->second.packets.size()) {
      continue;
    }
    UdpVideoPacket packet = it->second.packets[packet_index];
    if (packet.frame_packet_index != packet_index) {
      continue;
    }
    mark_repair_resend(&packet);
    out.push_back(std::move(packet));
  }
  return out;
}

std::vector<UdpVideoPacket> UdpVideoRepairCache::lookup_frame(UdpVideoStreamId stream,
                                                              uint32_t frame_sequence) const {
  const auto it = frames_.find(FrameKey{stream, frame_sequence});
  if (it == frames_.end()) {
    return {};
  }

  std::vector<UdpVideoPacket> out = it->second.packets;
  for (auto& packet : out) {
    mark_repair_resend(&packet);
  }
  return out;
}

void UdpVideoRepairCache::evict_expired(int64_t now_ns) {
  if (limits_.max_age_ns < 0) {
    return;
  }

  std::vector<FrameKey> expired;
  for (const auto& entry : frames_) {
    if (now_ns >= entry.second.stored_ns && now_ns - entry.second.stored_ns > limits_.max_age_ns) {
      expired.push_back(entry.first);
    }
  }

  for (const auto& key : expired) {
    const auto it = frames_.find(key);
    if (it != frames_.end()) {
      remove_frame(it);
    }
  }
}

bool UdpVideoRepairCache::build_cached_frame(const std::vector<UdpVideoPacket>& packets,
                                             int64_t now_ns,
                                             uint64_t insertion_order,
                                             CachedFrame* out) {
  if (packets.empty() || out == nullptr) {
    return false;
  }

  const UdpVideoStreamId stream = packets.front().stream_id;
  const uint32_t frame_sequence = packets.front().frame_sequence;
  const uint16_t packet_count = packets.front().frame_packet_count;
  const uint32_t frame_byte_length = packets.front().frame_byte_length;
  const uint32_t codec_header_length = packets.front().codec_header_length;
  const uint16_t session_id = packets.front().session_id;
  if (packet_count == 0 || packets.size() != packet_count) {
    return false;
  }

  std::vector<bool> seen(packet_count, false);
  CachedFrame cached;
  cached.packets = packets;
  cached.stored_ns = now_ns;
  cached.insertion_order = insertion_order;
  std::sort(cached.packets.begin(), cached.packets.end(), [](const UdpVideoPacket& a,
                                                             const UdpVideoPacket& b) {
    return a.frame_packet_index < b.frame_packet_index;
  });

  for (const auto& packet : cached.packets) {
    if (packet.stream_id != stream || packet.frame_sequence != frame_sequence ||
        packet.frame_packet_count != packet_count || packet.frame_byte_length != frame_byte_length ||
        packet.codec_header_length != codec_header_length || packet.session_id != session_id ||
        packet.frame_packet_index >= packet_count || seen[packet.frame_packet_index]) {
      return false;
    }
    seen[packet.frame_packet_index] = true;
    cached.payload_bytes += packet.payload.size();
    cached.keyframe_or_csd = cached.keyframe_or_csd || packet_has_keyframe_or_csd(packet);
  }

  for (bool has_packet : seen) {
    if (!has_packet) {
      return false;
    }
  }

  *out = std::move(cached);
  return true;
}

void UdpVideoRepairCache::remove_frame(FrameMap::iterator it) {
  subtract_stream_bytes(it->first.stream, it->second.payload_bytes);
  total_payload_bytes_ -= std::min(total_payload_bytes_, it->second.payload_bytes);
  frames_.erase(it);
}

void UdpVideoRepairCache::enforce_caps() {
  for (;;) {
    bool evicted = false;
    for (const UdpVideoStreamId stream : {UdpVideoStreamId::Road,
                                          UdpVideoStreamId::Wide,
                                          UdpVideoStreamId::Driver}) {
      const size_t before = frames_.size();
      enforce_stream_cap(stream);
      evicted = evicted || frames_.size() != before;
    }

    if (limits_.max_bytes_total > 0) {
      while (total_payload_bytes_ > limits_.max_bytes_total && !frames_.empty()) {
        auto candidate = choose_eviction_candidate_global();
        if (candidate == frames_.end()) {
          break;
        }
        remove_frame(candidate);
        evicted = true;
      }
    } else {
      while (!frames_.empty()) {
        remove_frame(frames_.begin());
        evicted = true;
      }
    }

    if (!evicted) {
      break;
    }
  }
}

void UdpVideoRepairCache::enforce_stream_cap(UdpVideoStreamId stream) {
  if (limits_.max_bytes_per_stream > 0) {
    while (stream_bytes(stream) > limits_.max_bytes_per_stream) {
      auto candidate = choose_eviction_candidate_for_stream(stream);
      if (candidate == frames_.end()) {
        break;
      }
      remove_frame(candidate);
    }
    return;
  }

  for (;;) {
    auto candidate = choose_eviction_candidate_for_stream(stream);
    if (candidate == frames_.end()) {
      break;
    }
    remove_frame(candidate);
  }
}

UdpVideoRepairCache::FrameMap::iterator UdpVideoRepairCache::choose_eviction_candidate_for_stream(
    UdpVideoStreamId stream) {
  const auto latest_keyframe = find_latest_keyframe(stream);
  auto oldest = frames_.end();
  auto oldest_unprotected = frames_.end();

  for (auto it = frames_.begin(); it != frames_.end(); ++it) {
    if (it->first.stream != stream) {
      continue;
    }
    if (oldest == frames_.end() || it->second.insertion_order < oldest->second.insertion_order) {
      oldest = it;
    }
    const bool protected_latest_keyframe = latest_keyframe != frames_.end() && it == latest_keyframe;
    if (!protected_latest_keyframe &&
        (oldest_unprotected == frames_.end() ||
         it->second.insertion_order < oldest_unprotected->second.insertion_order)) {
      oldest_unprotected = it;
    }
  }

  return oldest_unprotected != frames_.end() ? oldest_unprotected : oldest;
}

UdpVideoRepairCache::FrameMap::iterator UdpVideoRepairCache::choose_eviction_candidate_global() {
  auto oldest = frames_.end();
  auto oldest_unprotected = frames_.end();

  for (auto it = frames_.begin(); it != frames_.end(); ++it) {
    if (oldest == frames_.end() || it->second.insertion_order < oldest->second.insertion_order) {
      oldest = it;
    }
    const auto latest_keyframe = find_latest_keyframe(it->first.stream);
    const bool protected_latest_keyframe = latest_keyframe != frames_.end() && it == latest_keyframe;
    if (!protected_latest_keyframe &&
        (oldest_unprotected == frames_.end() ||
         it->second.insertion_order < oldest_unprotected->second.insertion_order)) {
      oldest_unprotected = it;
    }
  }

  return oldest_unprotected != frames_.end() ? oldest_unprotected : oldest;
}

UdpVideoRepairCache::FrameMap::const_iterator UdpVideoRepairCache::find_latest_keyframe(
    UdpVideoStreamId stream) const {
  auto latest = frames_.end();
  for (auto it = frames_.begin(); it != frames_.end(); ++it) {
    if (it->first.stream != stream || !it->second.keyframe_or_csd) {
      continue;
    }
    if (latest == frames_.end() || it->second.insertion_order > latest->second.insertion_order) {
      latest = it;
    }
  }
  return latest;
}

size_t UdpVideoRepairCache::stream_bytes(UdpVideoStreamId stream) const {
  const auto it = stream_payload_bytes_.find(stream);
  return it == stream_payload_bytes_.end() ? 0 : it->second;
}

void UdpVideoRepairCache::add_stream_bytes(UdpVideoStreamId stream, size_t bytes) {
  stream_payload_bytes_[stream] = stream_bytes(stream) + bytes;
}

void UdpVideoRepairCache::subtract_stream_bytes(UdpVideoStreamId stream, size_t bytes) {
  const auto it = stream_payload_bytes_.find(stream);
  if (it == stream_payload_bytes_.end()) {
    return;
  }
  if (bytes >= it->second) {
    stream_payload_bytes_.erase(it);
  } else {
    it->second -= bytes;
  }
}

}  // namespace commaview::video
