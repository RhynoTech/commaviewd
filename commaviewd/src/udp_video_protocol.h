#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace commaview::video {

constexpr uint32_t UDP_VIDEO_MAGIC = 0x43565550;  // CVUP
constexpr uint8_t UDP_VIDEO_VERSION = 1;
constexpr size_t UDP_VIDEO_MAX_DATAGRAM_BYTES = 1400;
constexpr size_t UDP_VIDEO_TARGET_PAYLOAD_BYTES = 1200;

enum class UdpVideoPacketType : uint8_t {
  Video = 1,
  Hello = 2,
  Heartbeat = 3,
  Policy = 4,
  RepairRequest = 5,
  RepairStatus = 6,
};

enum class UdpVideoStreamId : uint8_t {
  Road = 1,
  Wide = 2,
  Driver = 3,
};

constexpr uint16_t UDP_VIDEO_FLAG_KEYFRAME = 1u << 0;
constexpr uint16_t UDP_VIDEO_FLAG_CSD_PRESENT = 1u << 1;
constexpr uint16_t UDP_VIDEO_FLAG_FRAME_START = 1u << 2;
constexpr uint16_t UDP_VIDEO_FLAG_FRAME_END = 1u << 3;
constexpr uint16_t UDP_VIDEO_FLAG_REPAIR_RESEND = 1u << 4;

template <typename T>
class DecodeResult {
 public:
  static DecodeResult success(T value) {
    DecodeResult result;
    result.ok_ = true;
    result.value_ = std::move(value);
    return result;
  }

  static DecodeResult failure(std::string error) {
    DecodeResult result;
    result.ok_ = false;
    result.error_ = std::move(error);
    return result;
  }

  bool ok() const { return ok_; }

  const T& value() const {
    if (!ok_) {
      throw std::logic_error("DecodeResult has no value: " + error_);
    }
    return value_;
  }

  T& value() {
    if (!ok_) {
      throw std::logic_error("DecodeResult has no value: " + error_);
    }
    return value_;
  }

  const std::string& error() const { return error_; }

 private:
  bool ok_ = false;
  T value_{};
  std::string error_;
};

struct UdpVideoPacket {
  UdpVideoStreamId stream_id = UdpVideoStreamId::Road;
  uint16_t session_id = 0;
  uint64_t packet_sequence = 0;
  uint64_t timestamp_nanos = 0;
  uint32_t frame_sequence = 0;
  uint16_t frame_packet_index = 0;
  uint16_t frame_packet_count = 0;
  uint32_t frame_byte_offset = 0;
  uint32_t frame_byte_length = 0;
  uint32_t codec_header_length = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t flags = 0;
  std::vector<uint8_t> payload;
};

std::vector<uint8_t> encode_udp_video_packet(const UdpVideoPacket& packet);
DecodeResult<UdpVideoPacket> decode_udp_video_packet(const uint8_t* data, size_t size);

}  // namespace commaview::video
