#include "udp_video_packetizer.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace commaview::video {
namespace {

constexpr size_t kUdpVideoHeaderBytes = 60;

uint16_t base_flags_for_frame(const UdpVideoFrameForPacketizing& frame) {
  uint16_t flags = 0;
  if (frame.is_keyframe) {
    flags |= UDP_VIDEO_FLAG_KEYFRAME;
  }
  if (!frame.codec_header.empty()) {
    flags |= UDP_VIDEO_FLAG_CSD_PRESENT;
  }
  return flags;
}

std::vector<uint8_t> combine_frame_bytes(const UdpVideoFrameForPacketizing& frame) {
  std::vector<uint8_t> bytes;
  bytes.reserve(frame.codec_header.size() + frame.data.size());
  bytes.insert(bytes.end(), frame.codec_header.begin(), frame.codec_header.end());
  bytes.insert(bytes.end(), frame.data.begin(), frame.data.end());
  return bytes;
}

void validate_packetizer_inputs(const UdpVideoFrameForPacketizing& frame,
                                const uint64_t* next_packet_sequence,
                                size_t target_payload_bytes) {
  if (next_packet_sequence == nullptr) {
    throw std::invalid_argument("UDP video packetizer packet sequence pointer is null");
  }
  if (frame.data.empty()) {
    throw std::invalid_argument("UDP video packetizer frame data is empty");
  }
  if (target_payload_bytes == 0) {
    throw std::invalid_argument("UDP video packetizer target payload size is zero");
  }
  if (target_payload_bytes > UDP_VIDEO_MAX_DATAGRAM_BYTES - kUdpVideoHeaderBytes) {
    throw std::length_error("UDP video packetizer target payload exceeds max datagram size");
  }
  if (frame.codec_header.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("UDP video codec header is too large");
  }
  if (frame.codec_header.size() > std::numeric_limits<size_t>::max() - frame.data.size()) {
    throw std::length_error("UDP video frame is too large");
  }
  if (frame.codec_header.size() + frame.data.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("UDP video frame is too large");
  }
}

}  // namespace

std::vector<UdpVideoPacket> packetize_udp_video_frame(const UdpVideoFrameForPacketizing& frame,
                                                      uint64_t* next_packet_sequence,
                                                      size_t target_payload_bytes) {
  validate_packetizer_inputs(frame, next_packet_sequence, target_payload_bytes);

  const std::vector<uint8_t> frame_bytes = combine_frame_bytes(frame);
  const size_t packet_count_size =
      (frame_bytes.size() + target_payload_bytes - 1) / target_payload_bytes;
  if (packet_count_size == 0 || packet_count_size > std::numeric_limits<uint16_t>::max()) {
    throw std::length_error("UDP video frame requires too many packets");
  }

  const uint16_t packet_count = static_cast<uint16_t>(packet_count_size);
  const uint32_t frame_byte_length = static_cast<uint32_t>(frame_bytes.size());
  const uint32_t codec_header_length = static_cast<uint32_t>(frame.codec_header.size());
  const uint16_t base_flags = base_flags_for_frame(frame);

  std::vector<UdpVideoPacket> packets;
  packets.reserve(packet_count_size);
  for (size_t index = 0; index < packet_count_size; ++index) {
    const size_t offset = index * target_payload_bytes;
    const size_t payload_size = std::min(target_payload_bytes, frame_bytes.size() - offset);

    UdpVideoPacket packet;
    packet.stream_id = frame.stream_id;
    packet.session_id = frame.session_id;
    packet.packet_sequence = (*next_packet_sequence)++;
    packet.timestamp_nanos = frame.timestamp_nanos;
    packet.frame_sequence = frame.frame_sequence;
    packet.frame_packet_index = static_cast<uint16_t>(index);
    packet.frame_packet_count = packet_count;
    packet.frame_byte_offset = static_cast<uint32_t>(offset);
    packet.frame_byte_length = frame_byte_length;
    packet.codec_header_length = codec_header_length;
    packet.width = frame.width;
    packet.height = frame.height;
    packet.flags = base_flags;
    if (index == 0) {
      packet.flags |= UDP_VIDEO_FLAG_FRAME_START;
    }
    if (index + 1 == packet_count_size) {
      packet.flags |= UDP_VIDEO_FLAG_FRAME_END;
    }
    packet.payload.assign(frame_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                          frame_bytes.begin() + static_cast<std::ptrdiff_t>(offset + payload_size));
    packets.push_back(std::move(packet));
  }
  return packets;
}

}  // namespace commaview::video
