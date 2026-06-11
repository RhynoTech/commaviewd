#include "udp_video_protocol.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#define TEST(SuiteName, TestName) void SuiteName##_##TestName()
#define ASSERT_TRUE(expr) assert((expr))
#define ASSERT_FALSE(expr) assert(!(expr))
#define EXPECT_EQ(actual, expected) assert((actual) == (expected))

namespace {

constexpr size_t kUdpVideoMagicOffset = 0;
constexpr size_t kUdpVideoVersionOffset = 4;

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

}  // namespace

TEST(UdpVideoProtocolTest, EncodesAndDecodesVideoPacket) {
  const auto packet = sample_video_packet();

  const auto bytes = commaview::video::encode_udp_video_packet(packet);
  const auto decoded = commaview::video::decode_udp_video_packet(bytes.data(), bytes.size());

  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value().stream_id, packet.stream_id);
  EXPECT_EQ(decoded.value().frame_sequence, 7u);
  EXPECT_EQ(decoded.value().frame_packet_index, 1u);
  EXPECT_EQ(decoded.value().payload, packet.payload);
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
  auto packet = sample_video_packet();
  packet.frame_packet_index = 3;
  packet.frame_packet_count = 3;
  const auto bytes = commaview::video::encode_udp_video_packet(packet);

  const auto decoded = commaview::video::decode_udp_video_packet(bytes.data(), bytes.size());

  ASSERT_FALSE(decoded.ok());
}

TEST(UdpVideoProtocolTest, RejectsDatagramOverMaxUdpPayloadConstant) {
  std::vector<uint8_t> bytes(commaview::video::UDP_VIDEO_MAX_DATAGRAM_BYTES + 1);

  const auto decoded = commaview::video::decode_udp_video_packet(bytes.data(), bytes.size());

  ASSERT_FALSE(decoded.ok());
}

int main() {
  UdpVideoProtocolTest_EncodesAndDecodesVideoPacket();
  UdpVideoProtocolTest_RejectsWrongMagic();
  UdpVideoProtocolTest_RejectsWrongVersion();
  UdpVideoProtocolTest_RejectsPayloadLengthMismatch();
  UdpVideoProtocolTest_RejectsFramePacketIndexAtOrAfterPacketCount();
  UdpVideoProtocolTest_RejectsDatagramOverMaxUdpPayloadConstant();
  return 0;
}
