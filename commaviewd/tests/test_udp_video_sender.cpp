#include "udp_video_sender.h"

#include "udp_video_protocol.h"

#include <arpa/inet.h>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <vector>

#define TEST(SuiteName, TestName) void SuiteName##_##TestName()
#define ASSERT_TRUE(expr) assert((expr))
#define EXPECT_EQ(actual, expected) assert((actual) == (expected))

namespace {

using commaview::video::UDP_VIDEO_FLAG_REPAIR_RESEND;
using commaview::video::UdpVideoFrameForPacketizing;
using commaview::video::UdpVideoPacket;
using commaview::video::UdpVideoRepairRequest;
using commaview::video::UdpVideoSender;
using commaview::video::UdpVideoStreamId;

struct SentDatagram {
  std::vector<uint8_t> bytes;
  sockaddr_storage addr{};
  socklen_t addr_len = 0;
};

sockaddr_storage endpoint(uint16_t port) {
  sockaddr_storage storage{};
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  std::memcpy(&storage, &addr, sizeof(addr));
  return storage;
}

uint16_t endpoint_port(const sockaddr_storage& storage) {
  sockaddr_in addr{};
  std::memcpy(&addr, &storage, sizeof(addr));
  return ntohs(addr.sin_port);
}

UdpVideoFrameForPacketizing make_frame(UdpVideoStreamId stream = UdpVideoStreamId::Road,
                                       uint16_t session_id = 77,
                                       uint32_t frame_sequence = 10,
                                       size_t data_size = 180) {
  UdpVideoFrameForPacketizing frame;
  frame.stream_id = stream;
  frame.session_id = session_id;
  frame.frame_sequence = frame_sequence;
  frame.timestamp_nanos = 1000000ULL + frame_sequence;
  frame.width = 1928;
  frame.height = 1208;
  frame.is_keyframe = true;
  frame.codec_header = {0x01, 0x64, 0x00, 0x1f};
  frame.data.assign(data_size, 0);
  for (size_t i = 0; i < frame.data.size(); ++i) {
    frame.data[i] = static_cast<uint8_t>((frame_sequence + i) & 0xff);
  }
  return frame;
}

std::vector<UdpVideoPacket> decoded_packets(const std::vector<SentDatagram>& sends) {
  std::vector<UdpVideoPacket> packets;
  for (const auto& send : sends) {
    const auto decoded = commaview::video::decode_udp_video_packet(send.bytes.data(), send.bytes.size());
    ASSERT_TRUE(decoded.ok());
    packets.push_back(decoded.value());
  }
  return packets;
}

UdpVideoSender make_sender(std::vector<SentDatagram>* sends,
                           int fail_after_successes = -1,
                           ssize_t failure = -1,
                           int64_t client_timeout_ns = commaview::video::UDP_VIDEO_CLIENT_TIMEOUT_NS) {
  return UdpVideoSender(
      [sends, fail_after_successes, failure](const uint8_t* data,
                                             size_t size,
                                             const sockaddr_storage& addr,
                                             socklen_t addr_len) -> ssize_t {
        if (fail_after_successes >= 0 &&
            sends->size() >= static_cast<size_t>(fail_after_successes)) {
          return failure;
        }
        SentDatagram sent;
        sent.bytes.assign(data, data + size);
        sent.addr = addr;
        sent.addr_len = addr_len;
        sends->push_back(std::move(sent));
        return static_cast<ssize_t>(size);
      },
      {10000, 10000, 1000000},
      60,
      client_timeout_ns);
}

size_t payload_bytes(const std::vector<UdpVideoPacket>& packets) {
  size_t bytes = 0;
  for (const auto& packet : packets) {
    bytes += packet.payload.size();
  }
  return bytes;
}

}  // namespace

TEST(UdpVideoSenderTest, HelloRegistersEndpointAndFrameSendPacketizesToUdpEndpoint) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends);
  const auto addr = endpoint(41000);

  sender.note_client_hello(UdpVideoStreamId::Road, addr, sizeof(sockaddr_in), 77);
  const auto stats = sender.send_frame(make_frame(), 100);

  ASSERT_TRUE(sends.size() > 1);
  EXPECT_EQ(stats.packets_packetized, sends.size());
  EXPECT_EQ(stats.packets_sent, sends.size());
  EXPECT_EQ(stats.send_errors, 0U);
  EXPECT_EQ(endpoint_port(sends[0].addr), 41000U);
  EXPECT_EQ(sends[0].addr_len, static_cast<socklen_t>(sizeof(sockaddr_in)));

  const auto packets = decoded_packets(sends);
  for (size_t i = 0; i < packets.size(); ++i) {
    EXPECT_EQ(packets[i].stream_id, UdpVideoStreamId::Road);
    EXPECT_EQ(packets[i].session_id, 77U);
    EXPECT_EQ(packets[i].frame_sequence, 10U);
    EXPECT_EQ(packets[i].frame_packet_index, static_cast<uint16_t>(i));
    EXPECT_EQ(packets[i].frame_packet_count, static_cast<uint16_t>(packets.size()));
  }
}

TEST(UdpVideoSenderTest, SentPacketsEnterRepairCacheAndRepairRequestResendsWithFlag) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends);
  sender.note_client_hello(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77);
  const auto send_stats = sender.send_frame(make_frame(), 100);
  const size_t original_send_count = sends.size();

  UdpVideoRepairRequest request;
  request.stream_id = UdpVideoStreamId::Road;
  request.session_id = 77;
  request.frame_sequence = 10;
  request.packet_indexes = {1, 0};
  const auto repair_stats = sender.handle_repair_request(request, 200);

  EXPECT_EQ(repair_stats.requests, 1U);
  EXPECT_EQ(repair_stats.packets_resent, 2U);
  EXPECT_EQ(repair_stats.missing_count, 0U);
  EXPECT_EQ(sends.size(), original_send_count + 2);
  EXPECT_EQ(send_stats.packets_packetized, original_send_count);

  const auto packets = decoded_packets(sends);
  EXPECT_EQ(packets[original_send_count].frame_packet_index, 1U);
  EXPECT_EQ(packets[original_send_count + 1].frame_packet_index, 0U);
  EXPECT_EQ(packets[original_send_count].flags & UDP_VIDEO_FLAG_REPAIR_RESEND,
            UDP_VIDEO_FLAG_REPAIR_RESEND);
  EXPECT_EQ(packets[original_send_count + 1].flags & UDP_VIDEO_FLAG_REPAIR_RESEND,
            UDP_VIDEO_FLAG_REPAIR_RESEND);
}

TEST(UdpVideoSenderTest, SendFailuresIncrementUdpSendErrorCounters) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends, 1);
  sender.note_client_hello(UdpVideoStreamId::Wide, endpoint(41001), sizeof(sockaddr_in), 88);

  const auto stats = sender.send_frame(make_frame(UdpVideoStreamId::Wide, 88, 11, 180), 100);

  EXPECT_EQ(stats.packets_packetized > 1, true);
  EXPECT_EQ(stats.packets_sent, 1U);
  EXPECT_EQ(stats.send_errors, stats.packets_packetized - 1);
}

TEST(UdpVideoSenderTest, MissingRepairRequestIncrementsMissCounter) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends);
  sender.note_client_hello(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77);

  UdpVideoRepairRequest request;
  request.stream_id = UdpVideoStreamId::Road;
  request.session_id = 77;
  request.frame_sequence = 999;
  request.packet_indexes = {0};
  const auto stats = sender.handle_repair_request(request, 200);

  EXPECT_EQ(stats.requests, 1U);
  EXPECT_EQ(stats.packets_resent, 0U);
  EXPECT_EQ(stats.missing_count, 1U);
  EXPECT_EQ(sends.empty(), true);
}

TEST(UdpVideoSenderTest, MixedHitMissRepairRequestCountsOnlyUnresolvedUniquePackets) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends);
  sender.note_client_hello(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77);
  sender.send_frame(make_frame(UdpVideoStreamId::Road, 77, 10, 180), 100);
  const size_t original_send_count = sends.size();

  UdpVideoRepairRequest request;
  request.stream_id = UdpVideoStreamId::Road;
  request.session_id = 77;
  request.frame_sequence = 10;
  request.packet_indexes = {0, 0, 999};
  const auto stats = sender.handle_repair_request(request, 200);

  EXPECT_EQ(stats.requests, 1U);
  EXPECT_EQ(stats.packets_resent, 1U);
  EXPECT_EQ(stats.missing_count, 1U);
  EXPECT_EQ(sends.size(), original_send_count + 1);
  const auto packets = decoded_packets(sends);
  EXPECT_EQ(packets[original_send_count].frame_packet_index, 0U);
}

TEST(UdpVideoSenderTest, FrameSendWithoutClientIsDroppedWithoutFillingRepairCache) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends);

  const auto stats = sender.send_frame(make_frame(), 100);

  EXPECT_EQ(stats.dropped_no_client, 1U);
  EXPECT_EQ(stats.packets_packetized, 0U);
  EXPECT_EQ(stats.packets_sent, 0U);
  EXPECT_EQ(sends.empty(), true);

  UdpVideoRepairRequest request;
  request.stream_id = UdpVideoStreamId::Road;
  request.session_id = 77;
  request.frame_sequence = 10;
  const auto repair_stats = sender.handle_repair_request(request, 200);
  EXPECT_EQ(repair_stats.missing_count, 1U);
}

TEST(UdpVideoSenderTest, RepairRequestsAreSessionScoped) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends);
  sender.note_client_hello(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77);
  sender.send_frame(make_frame(UdpVideoStreamId::Road, 77, 10, 100), 100);
  const size_t original_send_count = sends.size();

  UdpVideoRepairRequest wrong_session;
  wrong_session.stream_id = UdpVideoStreamId::Road;
  wrong_session.session_id = 78;
  wrong_session.frame_sequence = 10;
  const auto stats = sender.handle_repair_request(wrong_session, 200);

  EXPECT_EQ(stats.requests, 1U);
  EXPECT_EQ(stats.packets_resent, 0U);
  EXPECT_EQ(stats.missing_count, 1U);
  EXPECT_EQ(sends.size(), original_send_count);
}

TEST(UdpVideoSenderTest, HelloForNewSessionReplacesEndpointForStream) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends);
  sender.note_client_hello(UdpVideoStreamId::Driver, endpoint(41000), sizeof(sockaddr_in), 77);
  sender.note_client_hello(UdpVideoStreamId::Driver, endpoint(42000), sizeof(sockaddr_in), 99);

  const auto stats = sender.send_frame(make_frame(UdpVideoStreamId::Driver, 77, 12, 60), 100);

  EXPECT_EQ(stats.packets_sent, sends.size());
  ASSERT_TRUE(!sends.empty());
  EXPECT_EQ(endpoint_port(sends[0].addr), 42000U);
  const auto packets = decoded_packets(sends);
  EXPECT_EQ(packets[0].session_id, 99U);
}

TEST(UdpVideoSenderTest, InvalidEndpointLengthsDoNotReplaceCurrentEndpoint) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends);
  sender.note_client_hello(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77);
  sender.note_client_hello(UdpVideoStreamId::Road, endpoint(42000), 0, 99);
  sender.note_client_hello(UdpVideoStreamId::Road,
                           endpoint(43000),
                           static_cast<socklen_t>(sizeof(sockaddr_storage) + 1),
                           100);

  const auto stats = sender.send_frame(make_frame(UdpVideoStreamId::Road, 77, 13, 60), 100);

  EXPECT_EQ(stats.packets_sent, sends.size());
  ASSERT_TRUE(!sends.empty());
  EXPECT_EQ(endpoint_port(sends[0].addr), 41000U);
  EXPECT_EQ(sends[0].addr_len, static_cast<socklen_t>(sizeof(sockaddr_in)));
  const auto packets = decoded_packets(sends);
  EXPECT_EQ(packets[0].session_id, 77U);
}

TEST(UdpVideoSenderTest, InvalidEndpointLengthBeforeValidHelloIsNotUsedForSends) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends);
  sender.note_client_hello(UdpVideoStreamId::Wide, endpoint(41001), 0, 88);

  const auto stats = sender.send_frame(make_frame(UdpVideoStreamId::Wide, 88, 14, 60), 100);

  EXPECT_EQ(stats.dropped_no_client, 1U);
  EXPECT_EQ(stats.packets_sent, 0U);
  EXPECT_EQ(sends.empty(), true);
}

TEST(UdpVideoSenderTest, PartialSizedUdpReturnCountsAsSendErrorNotSentPacket) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends, 0, 5);
  sender.note_client_hello(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77);

  const auto stats = sender.send_frame(make_frame(UdpVideoStreamId::Road, 77, 15, 60), 100);

  EXPECT_EQ(stats.packets_sent, 0U);
  EXPECT_EQ(stats.send_errors, stats.packets_packetized);
  EXPECT_EQ(sends.empty(), true);
}

TEST(UdpVideoSenderTest, NegativeUdpReturnCountsAsSendErrorNotSentPacket) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends, 0, -1);
  sender.note_client_hello(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77);

  const auto stats = sender.send_frame(make_frame(UdpVideoStreamId::Road, 77, 16, 60), 100);

  EXPECT_EQ(stats.packets_sent, 0U);
  EXPECT_EQ(stats.send_errors, stats.packets_packetized);
  EXPECT_EQ(sends.empty(), true);
}

TEST(UdpVideoSenderTest, SameFrameAndSessionAcrossDifferentStreamsStayIsolated) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends);
  sender.note_client_hello(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77);
  sender.note_client_hello(UdpVideoStreamId::Wide, endpoint(42000), sizeof(sockaddr_in), 77);
  sender.send_frame(make_frame(UdpVideoStreamId::Road, 77, 17, 70), 100);
  sender.send_frame(make_frame(UdpVideoStreamId::Wide, 77, 17, 90), 200);
  const size_t original_send_count = sends.size();

  UdpVideoRepairRequest request;
  request.stream_id = UdpVideoStreamId::Wide;
  request.session_id = 77;
  request.frame_sequence = 17;
  request.packet_indexes = {0};
  const auto stats = sender.handle_repair_request(request, 300);

  EXPECT_EQ(stats.packets_resent, 1U);
  EXPECT_EQ(stats.missing_count, 0U);
  EXPECT_EQ(sends.size(), original_send_count + 1);
  EXPECT_EQ(endpoint_port(sends[original_send_count].addr), 42000U);
  const auto packets = decoded_packets(sends);
  EXPECT_EQ(packets[original_send_count].stream_id, UdpVideoStreamId::Wide);
}

TEST(UdpVideoSenderTest, SendStatsExposeRepairCacheBytesAndHighWatermark) {
  std::vector<SentDatagram> sends;
  auto sender = UdpVideoSender(
      [&sends](const uint8_t* data,
               size_t size,
               const sockaddr_storage& addr,
               socklen_t addr_len) -> ssize_t {
        SentDatagram sent;
        sent.bytes.assign(data, data + size);
        sent.addr = addr;
        sent.addr_len = addr_len;
        sends.push_back(std::move(sent));
        return static_cast<ssize_t>(size);
      },
      {10000, 10000, 50},
      200);
  sender.note_client_hello(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77);

  const auto first_stats = sender.send_frame(make_frame(UdpVideoStreamId::Road, 77, 18, 80), 100);
  const auto first_packets = decoded_packets(sends);
  const size_t first_payload_bytes = payload_bytes(first_packets);
  EXPECT_EQ(first_stats.repair_cache_bytes, first_payload_bytes);
  EXPECT_EQ(first_stats.repair_cache_high_watermark_bytes, first_payload_bytes);

  const auto second_stats = sender.send_frame(make_frame(UdpVideoStreamId::Road, 77, 19, 40), 200);
  EXPECT_EQ(second_stats.repair_cache_bytes < second_stats.repair_cache_high_watermark_bytes, true);
  EXPECT_EQ(second_stats.repair_cache_high_watermark_bytes >= first_payload_bytes, true);
}

TEST(UdpVideoSenderTest, ClientExpiresAfterTimeoutAndCheckinRevivesIt) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends, -1, -1, 1000);
  sender.note_client_hello(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77, 0);

  ASSERT_TRUE(sender.has_active_client(UdpVideoStreamId::Road, 500));
  const auto fresh_stats = sender.send_frame(make_frame(), 500);
  EXPECT_EQ(fresh_stats.dropped_no_client, 0U);
  ASSERT_TRUE(fresh_stats.packets_sent > 0);

  ASSERT_TRUE(!sender.has_active_client(UdpVideoStreamId::Road, 2000));
  const size_t sends_before_expiry = sends.size();
  const auto expired_stats = sender.send_frame(make_frame(UdpVideoStreamId::Road, 77, 11, 60), 2000);
  EXPECT_EQ(expired_stats.dropped_no_client, 1U);
  EXPECT_EQ(sends.size(), sends_before_expiry);

  sender.note_client_hello(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77, 2100);
  const auto revived_stats = sender.send_frame(make_frame(UdpVideoStreamId::Road, 77, 12, 60), 2200);
  EXPECT_EQ(revived_stats.dropped_no_client, 0U);
  ASSERT_TRUE(revived_stats.packets_sent > 0);
}

TEST(UdpVideoSenderTest, PolicySuppressVideoDropsFramesUntilCleared) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends);
  sender.note_client_policy(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77, true, 100);

  ASSERT_TRUE(sender.has_active_client(UdpVideoStreamId::Road, 200));
  ASSERT_TRUE(sender.client_suppresses_video(UdpVideoStreamId::Road));
  const auto suppressed_stats = sender.send_frame(make_frame(), 200);
  EXPECT_EQ(suppressed_stats.dropped_suppressed, 1U);
  EXPECT_EQ(suppressed_stats.packets_packetized, 0U);
  EXPECT_EQ(sends.empty(), true);

  sender.note_client_policy(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77, false, 300);
  ASSERT_TRUE(!sender.client_suppresses_video(UdpVideoStreamId::Road));
  const auto resumed_stats = sender.send_frame(make_frame(UdpVideoStreamId::Road, 77, 11, 60), 400);
  EXPECT_EQ(resumed_stats.dropped_suppressed, 0U);
  ASSERT_TRUE(resumed_stats.packets_sent > 0);
}

TEST(UdpVideoSenderTest, HelloForNewSessionResetsSuppressFlag) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends);
  sender.note_client_policy(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77, true, 100);
  sender.note_client_hello(UdpVideoStreamId::Road, endpoint(42000), sizeof(sockaddr_in), 99, 200);

  ASSERT_TRUE(!sender.client_suppresses_video(UdpVideoStreamId::Road));
  const auto stats = sender.send_frame(make_frame(UdpVideoStreamId::Road, 99, 11, 60), 300);
  EXPECT_EQ(stats.dropped_suppressed, 0U);
  ASSERT_TRUE(stats.packets_sent > 0);
}

TEST(UdpVideoSenderTest, RepairRequestsStillServedWhileVideoSuppressed) {
  std::vector<SentDatagram> sends;
  auto sender = make_sender(&sends);
  sender.note_client_hello(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77, 100);
  sender.send_frame(make_frame(UdpVideoStreamId::Road, 77, 10, 100), 100);
  const size_t original_send_count = sends.size();
  sender.note_client_policy(UdpVideoStreamId::Road, endpoint(41000), sizeof(sockaddr_in), 77, true, 150);

  UdpVideoRepairRequest request;
  request.stream_id = UdpVideoStreamId::Road;
  request.session_id = 77;
  request.frame_sequence = 10;
  request.packet_indexes = {0};
  const auto stats = sender.handle_repair_request(request, 200);

  EXPECT_EQ(stats.packets_resent, 1U);
  EXPECT_EQ(stats.missing_count, 0U);
  EXPECT_EQ(sends.size(), original_send_count + 1);
}

int main() {
  UdpVideoSenderTest_HelloRegistersEndpointAndFrameSendPacketizesToUdpEndpoint();
  UdpVideoSenderTest_SentPacketsEnterRepairCacheAndRepairRequestResendsWithFlag();
  UdpVideoSenderTest_SendFailuresIncrementUdpSendErrorCounters();
  UdpVideoSenderTest_MissingRepairRequestIncrementsMissCounter();
  UdpVideoSenderTest_MixedHitMissRepairRequestCountsOnlyUnresolvedUniquePackets();
  UdpVideoSenderTest_FrameSendWithoutClientIsDroppedWithoutFillingRepairCache();
  UdpVideoSenderTest_RepairRequestsAreSessionScoped();
  UdpVideoSenderTest_HelloForNewSessionReplacesEndpointForStream();
  UdpVideoSenderTest_InvalidEndpointLengthsDoNotReplaceCurrentEndpoint();
  UdpVideoSenderTest_InvalidEndpointLengthBeforeValidHelloIsNotUsedForSends();
  UdpVideoSenderTest_PartialSizedUdpReturnCountsAsSendErrorNotSentPacket();
  UdpVideoSenderTest_NegativeUdpReturnCountsAsSendErrorNotSentPacket();
  UdpVideoSenderTest_SameFrameAndSessionAcrossDifferentStreamsStayIsolated();
  UdpVideoSenderTest_SendStatsExposeRepairCacheBytesAndHighWatermark();
  UdpVideoSenderTest_ClientExpiresAfterTimeoutAndCheckinRevivesIt();
  UdpVideoSenderTest_PolicySuppressVideoDropsFramesUntilCleared();
  UdpVideoSenderTest_HelloForNewSessionResetsSuppressFlag();
  UdpVideoSenderTest_RepairRequestsStillServedWhileVideoSuppressed();
  return 0;
}
