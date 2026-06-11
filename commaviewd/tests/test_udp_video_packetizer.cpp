#include "udp_video_packetizer.h"

#include "udp_video_protocol.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#define TEST(SuiteName, TestName) void SuiteName##_##TestName()
#define ASSERT_TRUE(expr) assert((expr))
#define EXPECT_EQ(actual, expected) assert((actual) == (expected))

namespace {

using commaview::video::UDP_VIDEO_FLAG_CSD_PRESENT;
using commaview::video::UDP_VIDEO_FLAG_FRAME_END;
using commaview::video::UDP_VIDEO_FLAG_FRAME_START;
using commaview::video::UDP_VIDEO_FLAG_KEYFRAME;
using commaview::video::UDP_VIDEO_MAX_DATAGRAM_BYTES;
using commaview::video::UdpVideoFrameForPacketizing;
using commaview::video::UdpVideoStreamId;

UdpVideoFrameForPacketizing sample_frame() {
  UdpVideoFrameForPacketizing frame;
  frame.stream_id = UdpVideoStreamId::Wide;
  frame.session_id = 0x4567;
  frame.frame_sequence = 123;
  frame.timestamp_nanos = 9876543210ULL;
  frame.width = 1928;
  frame.height = 1208;
  frame.is_keyframe = true;
  frame.codec_header = {0x01, 0x02, 0x03, 0x04, 0x05};
  frame.data.assign(2500, 0);
  for (size_t i = 0; i < frame.data.size(); ++i) {
    frame.data[i] = static_cast<uint8_t>((i * 17) & 0xff);
  }
  return frame;
}

std::vector<uint8_t> combined_bytes(const UdpVideoFrameForPacketizing& frame) {
  std::vector<uint8_t> bytes = frame.codec_header;
  bytes.insert(bytes.end(), frame.data.begin(), frame.data.end());
  return bytes;
}

template <typename Fn>
void assert_throws_invalid_argument(Fn fn) {
  try {
    fn();
    assert(false && "expected invalid_argument");
  } catch (const std::invalid_argument&) {
  }
}

template <typename Fn>
void assert_throws_length_error(Fn fn) {
  try {
    fn();
    assert(false && "expected length_error");
  } catch (const std::length_error&) {
  }
}

}  // namespace

TEST(UdpVideoPacketizerTest, SplitsFrameIntoMtuSafePacketsAndPreservesBytes) {
  const auto frame = sample_frame();
  uint64_t next_packet_sequence = 700;

  const auto packets = commaview::video::packetize_udp_video_frame(frame, &next_packet_sequence, 600);

  ASSERT_TRUE(packets.size() > 1);
  EXPECT_EQ(next_packet_sequence, 700 + packets.size());
  std::vector<uint8_t> reassembled;
  const auto expected_frame_bytes = combined_bytes(frame);
  for (size_t i = 0; i < packets.size(); ++i) {
    const auto& packet = packets[i];
    EXPECT_EQ(commaview::video::encode_udp_video_packet(packet).size() <= UDP_VIDEO_MAX_DATAGRAM_BYTES, true);
    EXPECT_EQ(packet.frame_packet_index, static_cast<uint16_t>(i));
    EXPECT_EQ(packet.frame_packet_count, static_cast<uint16_t>(packets.size()));
    EXPECT_EQ(packet.frame_byte_offset, reassembled.size());
    EXPECT_EQ(packet.frame_byte_length, static_cast<uint32_t>(expected_frame_bytes.size()));
    EXPECT_EQ(packet.codec_header_length, static_cast<uint32_t>(frame.codec_header.size()));
    EXPECT_EQ(packet.flags & UDP_VIDEO_FLAG_KEYFRAME, UDP_VIDEO_FLAG_KEYFRAME);
    EXPECT_EQ(packet.flags & UDP_VIDEO_FLAG_CSD_PRESENT, UDP_VIDEO_FLAG_CSD_PRESENT);
    if (i == 0) {
      EXPECT_EQ(packet.flags & UDP_VIDEO_FLAG_FRAME_START, UDP_VIDEO_FLAG_FRAME_START);
    } else {
      EXPECT_EQ(packet.flags & UDP_VIDEO_FLAG_FRAME_START, 0);
    }
    if (i + 1 == packets.size()) {
      EXPECT_EQ(packet.flags & UDP_VIDEO_FLAG_FRAME_END, UDP_VIDEO_FLAG_FRAME_END);
    } else {
      EXPECT_EQ(packet.flags & UDP_VIDEO_FLAG_FRAME_END, 0);
    }
    ASSERT_TRUE(packet.payload.size() <= 600);
    reassembled.insert(reassembled.end(), packet.payload.begin(), packet.payload.end());
  }
  EXPECT_EQ(reassembled, expected_frame_bytes);
}

TEST(UdpVideoPacketizerTest, RejectsEmptyData) {
  auto frame = sample_frame();
  frame.data.clear();
  uint64_t next_packet_sequence = 1;

  assert_throws_invalid_argument([&]() {
    (void)commaview::video::packetize_udp_video_frame(frame, &next_packet_sequence);
  });
}

TEST(UdpVideoPacketizerTest, RejectsNullPacketSequenceCounter) {
  const auto frame = sample_frame();

  assert_throws_invalid_argument([&]() {
    (void)commaview::video::packetize_udp_video_frame(frame, nullptr);
  });
}

TEST(UdpVideoPacketizerTest, RejectsInvalidStreamId) {
  auto frame = sample_frame();
  frame.stream_id = static_cast<UdpVideoStreamId>(0x7f);
  uint64_t next_packet_sequence = 1;

  assert_throws_invalid_argument([&]() {
    (void)commaview::video::packetize_udp_video_frame(frame, &next_packet_sequence);
  });
}

TEST(UdpVideoPacketizerTest, RejectsPacketCountOverflow) {
  auto frame = sample_frame();
  frame.codec_header.clear();
  frame.data.assign(65536, 0xab);
  uint64_t next_packet_sequence = 1;

  assert_throws_length_error([&]() {
    (void)commaview::video::packetize_udp_video_frame(frame, &next_packet_sequence, 1);
  });
}

TEST(UdpVideoPacketizerTest, RejectsZeroTargetPayloadSize) {
  const auto frame = sample_frame();
  uint64_t next_packet_sequence = 1;

  assert_throws_invalid_argument([&]() {
    (void)commaview::video::packetize_udp_video_frame(frame, &next_packet_sequence, 0);
  });
}

TEST(UdpVideoPacketizerTest, RejectsPayloadTargetThatCannotFitDatagram) {
  const auto frame = sample_frame();
  uint64_t next_packet_sequence = 1;

  assert_throws_length_error([&]() {
    (void)commaview::video::packetize_udp_video_frame(frame, &next_packet_sequence,
                                                      UDP_VIDEO_MAX_DATAGRAM_BYTES);
  });
}

TEST(UdpVideoPacketizerTest, PacketSequenceIncrementsFromSuppliedCounter) {
  const auto frame = sample_frame();
  uint64_t next_packet_sequence = 42;

  const auto packets = commaview::video::packetize_udp_video_frame(frame, &next_packet_sequence, 1000);

  for (size_t i = 0; i < packets.size(); ++i) {
    EXPECT_EQ(packets[i].packet_sequence, 42 + i);
  }
  EXPECT_EQ(next_packet_sequence, 42 + packets.size());
}

TEST(UdpVideoPacketizerTest, PropagatesFrameMetadataToEachPacket) {
  const auto frame = sample_frame();
  uint64_t next_packet_sequence = 10;

  const auto packets = commaview::video::packetize_udp_video_frame(frame, &next_packet_sequence, 700);

  for (const auto& packet : packets) {
    EXPECT_EQ(packet.stream_id, frame.stream_id);
    EXPECT_EQ(packet.session_id, frame.session_id);
    EXPECT_EQ(packet.frame_sequence, frame.frame_sequence);
    EXPECT_EQ(packet.timestamp_nanos, frame.timestamp_nanos);
    EXPECT_EQ(packet.width, frame.width);
    EXPECT_EQ(packet.height, frame.height);
    EXPECT_EQ(packet.codec_header_length, static_cast<uint32_t>(frame.codec_header.size()));
    EXPECT_EQ(packet.frame_byte_length, static_cast<uint32_t>(combined_bytes(frame).size()));
  }
}

TEST(UdpVideoPacketizerTest, AllowsFrameLargerThanManyPackets) {
  auto frame = sample_frame();
  frame.data.assign(5000, 0xcd);
  uint64_t next_packet_sequence = 10;

  const auto packets = commaview::video::packetize_udp_video_frame(frame, &next_packet_sequence, 333);

  ASSERT_TRUE(packets.size() > 10);
  EXPECT_EQ(packets.front().flags & UDP_VIDEO_FLAG_FRAME_START, UDP_VIDEO_FLAG_FRAME_START);
  EXPECT_EQ(packets.back().flags & UDP_VIDEO_FLAG_FRAME_END, UDP_VIDEO_FLAG_FRAME_END);
  EXPECT_EQ(next_packet_sequence, 10 + packets.size());
}

int main() {
  UdpVideoPacketizerTest_SplitsFrameIntoMtuSafePacketsAndPreservesBytes();
  UdpVideoPacketizerTest_RejectsEmptyData();
  UdpVideoPacketizerTest_RejectsNullPacketSequenceCounter();
  UdpVideoPacketizerTest_RejectsInvalidStreamId();
  UdpVideoPacketizerTest_RejectsPacketCountOverflow();
  UdpVideoPacketizerTest_RejectsZeroTargetPayloadSize();
  UdpVideoPacketizerTest_RejectsPayloadTargetThatCannotFitDatagram();
  UdpVideoPacketizerTest_PacketSequenceIncrementsFromSuppliedCounter();
  UdpVideoPacketizerTest_PropagatesFrameMetadataToEachPacket();
  UdpVideoPacketizerTest_AllowsFrameLargerThanManyPackets();
  return 0;
}
