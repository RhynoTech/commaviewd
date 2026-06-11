#include "udp_video_protocol.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#define TEST(SuiteName, TestName) void SuiteName##_##TestName()
#define ASSERT_TRUE(expr) assert((expr))
#define ASSERT_FALSE(expr) assert(!(expr))
#define EXPECT_EQ(actual, expected) assert((actual) == (expected))

namespace {

constexpr size_t kUdpVideoHeaderBytes = 60;
constexpr size_t kUdpVideoMagicOffset = 0;
constexpr size_t kUdpVideoVersionOffset = 4;
constexpr size_t kUdpVideoStreamIdOffset = 6;
constexpr size_t kUdpVideoReservedOffset = 7;
constexpr size_t kUdpVideoFlagsOffset = 10;
constexpr size_t kUdpVideoFramePacketIndexOffset = 32;
constexpr size_t kUdpVideoFramePacketCountOffset = 34;
constexpr size_t kUdpVideoFrameByteOffsetOffset = 36;

commaview::video::UdpVideoPacket sample_video_packet() {
  commaview::video::UdpVideoPacket packet;
  packet.stream_id = commaview::video::UdpVideoStreamId::Road;
  packet.session_id = 0x1234;
  packet.packet_sequence = 42;
  packet.frame_sequence = 7;
  packet.frame_packet_index = 1;
  packet.frame_packet_count = 3;
  packet.frame_byte_offset = 1200;
  packet.frame_byte_length = 3000;
  packet.codec_header_length = 96;
  packet.timestamp_nanos = 1000000000LL;
  packet.width = 1928;
  packet.height = 1208;
  packet.flags = commaview::video::UDP_VIDEO_FLAG_KEYFRAME |
                 commaview::video::UDP_VIDEO_FLAG_CSD_PRESENT;
  packet.payload = {1, 2, 3, 4};
  return packet;
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

TEST(UdpVideoProtocolTest, EncodesGoldenWireFormat) {
  commaview::video::UdpVideoPacket packet;
  packet.stream_id = commaview::video::UdpVideoStreamId::Wide;
  packet.session_id = 0x1234;
  packet.flags = commaview::video::UDP_VIDEO_FLAG_KEYFRAME |
                 commaview::video::UDP_VIDEO_FLAG_FRAME_END;
  packet.packet_sequence = 0x0102030405060708ULL;
  packet.timestamp_nanos = 0x1112131415161718ULL;
  packet.frame_sequence = 0x21222324;
  packet.frame_packet_index = 0x3132;
  packet.frame_packet_count = 0x3334;
  packet.frame_byte_offset = 0x41424344;
  packet.frame_byte_length = 0x51525354;
  packet.codec_header_length = 0x61626364;
  packet.width = 0x71727374;
  packet.height = 0x81828384;
  packet.payload = {0xde, 0xad};

  const std::vector<uint8_t> expected = {
      0x43, 0x56, 0x55, 0x50,
      0x01,
      0x01,
      0x02,
      0x00,
      0x12, 0x34,
      0x00, 0x09,
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
      0x21, 0x22, 0x23, 0x24,
      0x31, 0x32,
      0x33, 0x34,
      0x41, 0x42, 0x43, 0x44,
      0x51, 0x52, 0x53, 0x54,
      0x61, 0x62, 0x63, 0x64,
      0x71, 0x72, 0x73, 0x74,
      0x81, 0x82, 0x83, 0x84,
      0x00, 0x00, 0x00, 0x02,
      0xde, 0xad,
  };

  EXPECT_EQ(commaview::video::encode_udp_video_packet(packet), expected);
}

TEST(UdpVideoProtocolTest, EncodesAndDecodesAllVideoPacketFields) {
  const auto packet = sample_video_packet();

  const auto bytes = commaview::video::encode_udp_video_packet(packet);
  const auto decoded = commaview::video::decode_udp_video_packet(bytes.data(), bytes.size());

  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value().stream_id, packet.stream_id);
  EXPECT_EQ(decoded.value().session_id, packet.session_id);
  EXPECT_EQ(decoded.value().packet_sequence, packet.packet_sequence);
  EXPECT_EQ(decoded.value().timestamp_nanos, packet.timestamp_nanos);
  EXPECT_EQ(decoded.value().frame_sequence, packet.frame_sequence);
  EXPECT_EQ(decoded.value().frame_packet_index, packet.frame_packet_index);
  EXPECT_EQ(decoded.value().frame_packet_count, packet.frame_packet_count);
  EXPECT_EQ(decoded.value().frame_byte_offset, packet.frame_byte_offset);
  EXPECT_EQ(decoded.value().frame_byte_length, packet.frame_byte_length);
  EXPECT_EQ(decoded.value().codec_header_length, packet.codec_header_length);
  EXPECT_EQ(decoded.value().width, packet.width);
  EXPECT_EQ(decoded.value().height, packet.height);
  EXPECT_EQ(decoded.value().flags, packet.flags);
  EXPECT_EQ(decoded.value().payload, packet.payload);
}

TEST(UdpVideoProtocolTest, EncodesAndDecodesMaxDatagram) {
  auto packet = sample_video_packet();
  packet.frame_packet_index = 0;
  packet.frame_packet_count = 1;
  packet.frame_byte_offset = 0;
  packet.payload.assign(commaview::video::UDP_VIDEO_MAX_DATAGRAM_BYTES - kUdpVideoHeaderBytes, 0xab);
  packet.frame_byte_length = static_cast<uint32_t>(packet.payload.size());

  const auto bytes = commaview::video::encode_udp_video_packet(packet);
  EXPECT_EQ(bytes.size(), commaview::video::UDP_VIDEO_MAX_DATAGRAM_BYTES);

  const auto decoded = commaview::video::decode_udp_video_packet(bytes.data(), bytes.size());
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value().payload.size(), packet.payload.size());
}

TEST(UdpVideoProtocolTest, RejectsWrongMagic) {
  auto bytes = commaview::video::encode_udp_video_packet(sample_video_packet());
  bytes[kUdpVideoMagicOffset] ^= 0xff;

  const auto decoded = commaview::video::decode_udp_video_packet(bytes.data(), bytes.size());

  ASSERT_FALSE(decoded.ok());
}

TEST(UdpVideoProtocolTest, RejectsWrongVersion) {
  auto bytes = commaview::video::encode_udp_video_packet(sample_video_packet());
  bytes[kUdpVideoVersionOffset] = commaview::video::UDP_VIDEO_VERSION + 1;

  const auto decoded = commaview::video::decode_udp_video_packet(bytes.data(), bytes.size());

  ASSERT_FALSE(decoded.ok());
}

TEST(UdpVideoProtocolTest, RejectsPayloadLengthMismatch) {
  auto bytes = commaview::video::encode_udp_video_packet(sample_video_packet());
  bytes.pop_back();

  const auto decoded = commaview::video::decode_udp_video_packet(bytes.data(), bytes.size());

  ASSERT_FALSE(decoded.ok());
}

TEST(UdpVideoProtocolTest, RejectsFramePacketIndexAtOrAfterPacketCount) {
  auto bytes = commaview::video::encode_udp_video_packet(sample_video_packet());
  bytes[kUdpVideoFramePacketIndexOffset] = 0;
  bytes[kUdpVideoFramePacketIndexOffset + 1] = 3;
  bytes[kUdpVideoFramePacketCountOffset] = 0;
  bytes[kUdpVideoFramePacketCountOffset + 1] = 3;

  const auto decoded = commaview::video::decode_udp_video_packet(bytes.data(), bytes.size());

  ASSERT_FALSE(decoded.ok());
}

TEST(UdpVideoProtocolTest, RejectsDatagramOverMaxUdpPayloadConstant) {
  std::vector<uint8_t> bytes(commaview::video::UDP_VIDEO_MAX_DATAGRAM_BYTES + 1);

  const auto decoded = commaview::video::decode_udp_video_packet(bytes.data(), bytes.size());

  ASSERT_FALSE(decoded.ok());
}

TEST(UdpVideoProtocolTest, RejectsNonzeroReservedByte) {
  auto bytes = commaview::video::encode_udp_video_packet(sample_video_packet());
  bytes[kUdpVideoReservedOffset] = 1;

  const auto decoded = commaview::video::decode_udp_video_packet(bytes.data(), bytes.size());

  ASSERT_FALSE(decoded.ok());
}

TEST(UdpVideoProtocolTest, RejectsUnknownFlags) {
  auto bytes = commaview::video::encode_udp_video_packet(sample_video_packet());
  bytes[kUdpVideoFlagsOffset] = 0x80;
  bytes[kUdpVideoFlagsOffset + 1] = 0x00;

  const auto decoded = commaview::video::decode_udp_video_packet(bytes.data(), bytes.size());

  ASSERT_FALSE(decoded.ok());
}

TEST(UdpVideoProtocolTest, RejectsZeroFramePacketCount) {
  auto bytes = commaview::video::encode_udp_video_packet(sample_video_packet());
  bytes[kUdpVideoFramePacketCountOffset] = 0;
  bytes[kUdpVideoFramePacketCountOffset + 1] = 0;

  const auto decoded = commaview::video::decode_udp_video_packet(bytes.data(), bytes.size());

  ASSERT_FALSE(decoded.ok());
}

TEST(UdpVideoProtocolTest, RejectsInvalidStreamId) {
  auto bytes = commaview::video::encode_udp_video_packet(sample_video_packet());
  bytes[kUdpVideoStreamIdOffset] = 0x7f;

  const auto decoded = commaview::video::decode_udp_video_packet(bytes.data(), bytes.size());

  ASSERT_FALSE(decoded.ok());
}

TEST(UdpVideoProtocolTest, RejectsFrameByteRangeOverflow) {
  auto bytes = commaview::video::encode_udp_video_packet(sample_video_packet());
  bytes[kUdpVideoFrameByteOffsetOffset] = 0xff;
  bytes[kUdpVideoFrameByteOffsetOffset + 1] = 0xff;
  bytes[kUdpVideoFrameByteOffsetOffset + 2] = 0xff;
  bytes[kUdpVideoFrameByteOffsetOffset + 3] = 0xff;

  const auto decoded = commaview::video::decode_udp_video_packet(bytes.data(), bytes.size());

  ASSERT_FALSE(decoded.ok());
}

TEST(UdpVideoProtocolTest, EncodeRejectsInvalidStreamId) {
  auto packet = sample_video_packet();
  packet.stream_id = static_cast<commaview::video::UdpVideoStreamId>(0x7f);

  assert_throws_invalid_argument([&packet]() {
    (void)commaview::video::encode_udp_video_packet(packet);
  });
}

TEST(UdpVideoProtocolTest, EncodeRejectsUnknownFlags) {
  auto packet = sample_video_packet();
  packet.flags = 0x8000;

  assert_throws_invalid_argument([&packet]() {
    (void)commaview::video::encode_udp_video_packet(packet);
  });
}

TEST(UdpVideoProtocolTest, EncodeRejectsZeroFramePacketCount) {
  auto packet = sample_video_packet();
  packet.frame_packet_count = 0;

  assert_throws_invalid_argument([&packet]() {
    (void)commaview::video::encode_udp_video_packet(packet);
  });
}

TEST(UdpVideoProtocolTest, EncodeRejectsFramePacketIndexAtOrAfterPacketCount) {
  auto packet = sample_video_packet();
  packet.frame_packet_index = packet.frame_packet_count;

  assert_throws_invalid_argument([&packet]() {
    (void)commaview::video::encode_udp_video_packet(packet);
  });
}

TEST(UdpVideoProtocolTest, EncodeRejectsFrameByteRangeOverflow) {
  auto packet = sample_video_packet();
  packet.frame_byte_offset = packet.frame_byte_length - 1;
  packet.payload = {1, 2};

  assert_throws_invalid_argument([&packet]() {
    (void)commaview::video::encode_udp_video_packet(packet);
  });
}

TEST(UdpVideoProtocolTest, EncodeRejectsOversizedDatagram) {
  auto packet = sample_video_packet();
  packet.payload.assign(commaview::video::UDP_VIDEO_MAX_DATAGRAM_BYTES - kUdpVideoHeaderBytes + 1, 0xab);
  packet.frame_byte_offset = 0;
  packet.frame_byte_length = static_cast<uint32_t>(packet.payload.size());

  assert_throws_length_error([&packet]() {
    (void)commaview::video::encode_udp_video_packet(packet);
  });
}

int main() {
  UdpVideoProtocolTest_EncodesGoldenWireFormat();
  UdpVideoProtocolTest_EncodesAndDecodesAllVideoPacketFields();
  UdpVideoProtocolTest_EncodesAndDecodesMaxDatagram();
  UdpVideoProtocolTest_RejectsWrongMagic();
  UdpVideoProtocolTest_RejectsWrongVersion();
  UdpVideoProtocolTest_RejectsPayloadLengthMismatch();
  UdpVideoProtocolTest_RejectsFramePacketIndexAtOrAfterPacketCount();
  UdpVideoProtocolTest_RejectsDatagramOverMaxUdpPayloadConstant();
  UdpVideoProtocolTest_RejectsNonzeroReservedByte();
  UdpVideoProtocolTest_RejectsUnknownFlags();
  UdpVideoProtocolTest_RejectsZeroFramePacketCount();
  UdpVideoProtocolTest_RejectsInvalidStreamId();
  UdpVideoProtocolTest_RejectsFrameByteRangeOverflow();
  UdpVideoProtocolTest_EncodeRejectsInvalidStreamId();
  UdpVideoProtocolTest_EncodeRejectsUnknownFlags();
  UdpVideoProtocolTest_EncodeRejectsZeroFramePacketCount();
  UdpVideoProtocolTest_EncodeRejectsFramePacketIndexAtOrAfterPacketCount();
  UdpVideoProtocolTest_EncodeRejectsFrameByteRangeOverflow();
  UdpVideoProtocolTest_EncodeRejectsOversizedDatagram();
  return 0;
}
