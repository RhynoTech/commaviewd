#include "udp_video_repair_cache.h"

#include "udp_video_packetizer.h"
#include "udp_video_protocol.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#define TEST(SuiteName, TestName) void SuiteName##_##TestName()
#define ASSERT_TRUE(expr) assert((expr))
#define EXPECT_EQ(actual, expected) assert((actual) == (expected))

namespace {

using commaview::video::UDP_VIDEO_FLAG_REPAIR_RESEND;
using commaview::video::UdpVideoFrameForPacketizing;
using commaview::video::UdpVideoPacket;
using commaview::video::UdpVideoRepairCache;
using commaview::video::UdpVideoStreamId;

UdpVideoFrameForPacketizing make_frame(UdpVideoStreamId stream,
                                       uint16_t session_id,
                                       uint32_t frame_sequence,
                                       bool keyframe,
                                       size_t data_size) {
  UdpVideoFrameForPacketizing frame;
  frame.stream_id = stream;
  frame.session_id = session_id;
  frame.frame_sequence = frame_sequence;
  frame.timestamp_nanos = 1000000ULL + frame_sequence;
  frame.width = 1928;
  frame.height = 1208;
  frame.is_keyframe = keyframe;
  if (keyframe) {
    frame.codec_header = {0x01, 0x64, 0x00, 0x1f};
  }
  frame.data.assign(data_size, 0);
  for (size_t i = 0; i < frame.data.size(); ++i) {
    frame.data[i] = static_cast<uint8_t>((frame_sequence + i) & 0xff);
  }
  return frame;
}

std::vector<UdpVideoPacket> packetize(UdpVideoFrameForPacketizing frame, size_t target_payload = 50) {
  uint64_t next_packet_sequence = 1000 + frame.session_id * 100 + frame.frame_sequence * 10;
  return commaview::video::packetize_udp_video_frame(frame, &next_packet_sequence, target_payload);
}

size_t payload_bytes(const std::vector<UdpVideoPacket>& packets) {
  size_t bytes = 0;
  for (const auto& packet : packets) {
    bytes += packet.payload.size();
  }
  return bytes;
}

}  // namespace

TEST(UdpVideoRepairCacheTest, StoresPacketizedFrameAndReturnsRequestedRepairPackets) {
  const auto packets = packetize(make_frame(UdpVideoStreamId::Road, 77, 10, true, 180), 60);
  UdpVideoRepairCache cache({10000, 10000, 1000000});

  cache.store(packets, 100);
  const auto repaired = cache.lookup(UdpVideoStreamId::Road, 77, 10, {2, 0, 99});

  EXPECT_EQ(repaired.size(), 2U);
  EXPECT_EQ(repaired[0].frame_packet_index, 2U);
  EXPECT_EQ(repaired[1].frame_packet_index, 0U);
  EXPECT_EQ(repaired[0].flags & UDP_VIDEO_FLAG_REPAIR_RESEND, UDP_VIDEO_FLAG_REPAIR_RESEND);
  EXPECT_EQ(repaired[1].flags & UDP_VIDEO_FLAG_REPAIR_RESEND, UDP_VIDEO_FLAG_REPAIR_RESEND);
  EXPECT_EQ(packets[0].flags & UDP_VIDEO_FLAG_REPAIR_RESEND, 0);
}

TEST(UdpVideoRepairCacheTest, LookupFrameReturnsAllPacketsInFrameOrder) {
  const auto packets = packetize(make_frame(UdpVideoStreamId::Wide, 77, 11, true, 140), 55);
  UdpVideoRepairCache cache({10000, 10000, 1000000});

  cache.store(packets, 100);
  const auto repaired = cache.lookup_frame(UdpVideoStreamId::Wide, 77, 11);

  EXPECT_EQ(repaired.size(), packets.size());
  for (size_t i = 0; i < repaired.size(); ++i) {
    EXPECT_EQ(repaired[i].frame_packet_index, static_cast<uint16_t>(i));
    EXPECT_EQ(repaired[i].payload, packets[i].payload);
    EXPECT_EQ(repaired[i].flags & UDP_VIDEO_FLAG_REPAIR_RESEND, UDP_VIDEO_FLAG_REPAIR_RESEND);
  }
}

TEST(UdpVideoRepairCacheTest, DuplicateStoreReplacesExistingFrame) {
  auto first = packetize(make_frame(UdpVideoStreamId::Driver, 77, 12, false, 40), 100);
  auto replacement = packetize(make_frame(UdpVideoStreamId::Driver, 77, 12, false, 90), 100);
  replacement[0].packet_sequence = 9999;
  UdpVideoRepairCache cache({10000, 10000, 1000000});

  cache.store(first, 100);
  cache.store(replacement, 200);
  const auto repaired = cache.lookup_frame(UdpVideoStreamId::Driver, 77, 12);

  EXPECT_EQ(repaired.size(), replacement.size());
  EXPECT_EQ(repaired[0].packet_sequence, 9999ULL);
  EXPECT_EQ(repaired[0].frame_byte_length, replacement[0].frame_byte_length);
  EXPECT_EQ(repaired[0].payload, replacement[0].payload);
}

TEST(UdpVideoRepairCacheTest, IgnoresEmptyStores) {
  UdpVideoRepairCache cache({10000, 10000, 1000000});

  cache.store({}, 100);

  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Road, 77, 1).empty(), true);
}

TEST(UdpVideoRepairCacheTest, TotalByteCapEvictsOldestFrames) {
  const auto first = packetize(make_frame(UdpVideoStreamId::Road, 77, 20, false, 80), 200);
  const auto second = packetize(make_frame(UdpVideoStreamId::Wide, 77, 21, false, 80), 200);
  UdpVideoRepairCache cache({10000, payload_bytes(second), 1000000});

  cache.store(first, 100);
  cache.store(second, 200);

  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Road, 77, 20).empty(), true);
  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Wide, 77, 21).empty(), false);
}

TEST(UdpVideoRepairCacheTest, PerStreamByteCapEvictsOldestFramesInThatStream) {
  const auto first = packetize(make_frame(UdpVideoStreamId::Road, 77, 30, false, 70), 200);
  const auto second = packetize(make_frame(UdpVideoStreamId::Road, 77, 31, false, 70), 200);
  const auto other_stream = packetize(make_frame(UdpVideoStreamId::Driver, 77, 32, false, 70), 200);
  UdpVideoRepairCache cache({payload_bytes(second), 10000, 1000000});

  cache.store(first, 100);
  cache.store(other_stream, 150);
  cache.store(second, 200);

  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Road, 77, 30).empty(), true);
  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Road, 77, 31).empty(), false);
  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Driver, 77, 32).empty(), false);
}

TEST(UdpVideoRepairCacheTest, TimeCapEvictsExpiredFrames) {
  const auto old_frame = packetize(make_frame(UdpVideoStreamId::Road, 77, 40, false, 40), 200);
  const auto fresh_frame = packetize(make_frame(UdpVideoStreamId::Road, 77, 41, false, 40), 200);
  UdpVideoRepairCache cache({10000, 10000, 50});

  cache.store(old_frame, 100);
  cache.store(fresh_frame, 140);
  cache.evict_expired(151);

  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Road, 77, 40).empty(), true);
  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Road, 77, 41).empty(), false);
}

TEST(UdpVideoRepairCacheTest, PerStreamFrameCapEvictsOldestFramesInThatStream) {
  const auto first = packetize(make_frame(UdpVideoStreamId::Road, 77, 45, false, 40), 200);
  const auto second = packetize(make_frame(UdpVideoStreamId::Road, 77, 46, false, 40), 200);
  const auto third = packetize(make_frame(UdpVideoStreamId::Road, 77, 47, false, 40), 200);
  const auto other_stream = packetize(make_frame(UdpVideoStreamId::Driver, 77, 48, false, 40), 200);
  UdpVideoRepairCache::Limits limits{10000, 10000, 1000000};
  limits.max_frames_per_stream = 2;
  UdpVideoRepairCache cache(limits);

  cache.store(first, 100);
  cache.store(other_stream, 150);
  cache.store(second, 200);
  cache.store(third, 300);

  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Road, 77, 45).empty(), true);
  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Road, 77, 46).empty(), false);
  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Road, 77, 47).empty(), false);
  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Driver, 77, 48).empty(), false);
}

TEST(UdpVideoRepairCacheTest, RetainsLatestKeyframeWhenOlderFramesCanBeEvicted) {
  const auto old_keyframe = packetize(make_frame(UdpVideoStreamId::Wide, 77, 50, true, 60), 200);
  const auto older_non_keyframe = packetize(make_frame(UdpVideoStreamId::Wide, 77, 51, false, 60), 200);
  const auto latest_keyframe = packetize(make_frame(UdpVideoStreamId::Wide, 77, 52, true, 60), 200);
  UdpVideoRepairCache cache({payload_bytes(latest_keyframe), 10000, 1000000});

  cache.store(old_keyframe, 100);
  cache.store(older_non_keyframe, 200);
  cache.store(latest_keyframe, 300);

  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Wide, 77, 50).empty(), true);
  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Wide, 77, 51).empty(), true);
  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Wide, 77, 52).empty(), false);
}

TEST(UdpVideoRepairCacheTest, SameStreamAndFrameSequenceAreIsolatedBySession) {
  auto first_session = packetize(make_frame(UdpVideoStreamId::Road, 77, 60, false, 70), 200);
  auto second_session = packetize(make_frame(UdpVideoStreamId::Road, 88, 60, false, 90), 200);
  first_session[0].payload[0] = 0x11;
  second_session[0].payload[0] = 0x22;
  UdpVideoRepairCache cache({10000, 10000, 1000000});

  cache.store(first_session, 100);
  cache.store(second_session, 200);

  const auto first_repaired = cache.lookup_frame(UdpVideoStreamId::Road, 77, 60);
  const auto second_repaired = cache.lookup_frame(UdpVideoStreamId::Road, 88, 60);
  EXPECT_EQ(first_repaired.size(), first_session.size());
  EXPECT_EQ(second_repaired.size(), second_session.size());
  EXPECT_EQ(first_repaired[0].session_id, 77U);
  EXPECT_EQ(second_repaired[0].session_id, 88U);
  EXPECT_EQ(first_repaired[0].payload[0], 0x11);
  EXPECT_EQ(second_repaired[0].payload[0], 0x22);
}

TEST(UdpVideoRepairCacheTest, RejectsFrameWithNonContiguousByteOffsets) {
  auto packets = packetize(make_frame(UdpVideoStreamId::Road, 77, 70, false, 120), 50);
  packets[1].frame_byte_offset += 1;
  UdpVideoRepairCache cache({10000, 10000, 1000000});

  cache.store(packets, 100);

  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Road, 77, 70).empty(), true);
}

TEST(UdpVideoRepairCacheTest, RejectsFrameWithCodecHeaderLongerThanFrame) {
  auto packets = packetize(make_frame(UdpVideoStreamId::Wide, 77, 71, false, 40), 100);
  packets[0].codec_header_length = packets[0].frame_byte_length + 1;
  UdpVideoRepairCache cache({10000, 10000, 1000000});

  cache.store(packets, 100);

  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Wide, 77, 71).empty(), true);
}

TEST(UdpVideoRepairCacheTest, RejectsFrameWhosePayloadBytesDoNotReachFrameLength) {
  auto packets = packetize(make_frame(UdpVideoStreamId::Driver, 77, 72, false, 100), 200);
  packets[0].frame_byte_length += 1;
  UdpVideoRepairCache cache({10000, 10000, 1000000});

  cache.store(packets, 100);

  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Driver, 77, 72).empty(), true);
}

TEST(UdpVideoRepairCacheTest, RejectedFrameDoesNotChangeEvictionAccounting) {
  const auto valid = packetize(make_frame(UdpVideoStreamId::Road, 77, 73, false, 60), 200);
  auto malformed = packetize(make_frame(UdpVideoStreamId::Road, 77, 74, false, 60), 200);
  malformed[0].frame_byte_length += 1;
  UdpVideoRepairCache cache({payload_bytes(valid), payload_bytes(valid), 1000000});

  cache.store(malformed, 100);
  cache.store(valid, 200);

  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Road, 77, 74).empty(), true);
  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Road, 77, 73).empty(), false);
}

TEST(UdpVideoRepairCacheTest, DuplicateReplacementUpdatesByteAccountingExactly) {
  const auto first = packetize(make_frame(UdpVideoStreamId::Road, 77, 80, false, 40), 200);
  const auto replacement = packetize(make_frame(UdpVideoStreamId::Road, 77, 80, false, 90), 200);
  const auto other = packetize(make_frame(UdpVideoStreamId::Road, 77, 81, false, 30), 200);
  UdpVideoRepairCache cache({payload_bytes(replacement) + payload_bytes(other),
                             payload_bytes(replacement) + payload_bytes(other),
                             1000000});

  cache.store(first, 100);
  cache.store(replacement, 200);
  cache.store(other, 300);

  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Road, 77, 80).empty(), false);
  EXPECT_EQ(cache.lookup_frame(UdpVideoStreamId::Road, 77, 81).empty(), false);
}

TEST(UdpVideoRepairCacheTest, ExposesCurrentBytesAndHighWaterBytes) {
  const auto first = packetize(make_frame(UdpVideoStreamId::Road, 77, 90, false, 80), 200);
  const auto second = packetize(make_frame(UdpVideoStreamId::Road, 77, 91, false, 40), 200);
  UdpVideoRepairCache cache({10000, 10000, 1000000});

  cache.store(first, 100);
  EXPECT_EQ(cache.total_payload_bytes(), payload_bytes(first));
  EXPECT_EQ(cache.high_water_payload_bytes(), payload_bytes(first));

  cache.store(second, 200);
  EXPECT_EQ(cache.total_payload_bytes(), payload_bytes(first) + payload_bytes(second));
  EXPECT_EQ(cache.high_water_payload_bytes(), payload_bytes(first) + payload_bytes(second));
}

TEST(UdpVideoRepairCacheTest, HighWaterBytesDoNotShrinkAfterEviction) {
  const auto first = packetize(make_frame(UdpVideoStreamId::Road, 77, 92, false, 80), 200);
  const auto second = packetize(make_frame(UdpVideoStreamId::Road, 77, 93, false, 40), 200);
  UdpVideoRepairCache cache({10000, payload_bytes(first) + payload_bytes(second), 50});

  cache.store(first, 100);
  cache.store(second, 120);
  const size_t high_water = cache.high_water_payload_bytes();
  cache.evict_expired(200);

  EXPECT_EQ(cache.total_payload_bytes(), 0U);
  EXPECT_EQ(cache.high_water_payload_bytes(), high_water);
}

int main() {
  UdpVideoRepairCacheTest_StoresPacketizedFrameAndReturnsRequestedRepairPackets();
  UdpVideoRepairCacheTest_LookupFrameReturnsAllPacketsInFrameOrder();
  UdpVideoRepairCacheTest_DuplicateStoreReplacesExistingFrame();
  UdpVideoRepairCacheTest_IgnoresEmptyStores();
  UdpVideoRepairCacheTest_TotalByteCapEvictsOldestFrames();
  UdpVideoRepairCacheTest_PerStreamByteCapEvictsOldestFramesInThatStream();
  UdpVideoRepairCacheTest_TimeCapEvictsExpiredFrames();
  UdpVideoRepairCacheTest_PerStreamFrameCapEvictsOldestFramesInThatStream();
  UdpVideoRepairCacheTest_RetainsLatestKeyframeWhenOlderFramesCanBeEvicted();
  UdpVideoRepairCacheTest_SameStreamAndFrameSequenceAreIsolatedBySession();
  UdpVideoRepairCacheTest_RejectsFrameWithNonContiguousByteOffsets();
  UdpVideoRepairCacheTest_RejectsFrameWithCodecHeaderLongerThanFrame();
  UdpVideoRepairCacheTest_RejectsFrameWhosePayloadBytesDoNotReachFrameLength();
  UdpVideoRepairCacheTest_RejectedFrameDoesNotChangeEvictionAccounting();
  UdpVideoRepairCacheTest_DuplicateReplacementUpdatesByteAccountingExactly();
  UdpVideoRepairCacheTest_ExposesCurrentBytesAndHighWaterBytes();
  UdpVideoRepairCacheTest_HighWaterBytesDoNotShrinkAfterEviction();
  return 0;
}
