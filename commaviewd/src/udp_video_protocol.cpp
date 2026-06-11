#include "udp_video_protocol.h"

#include <limits>
#include <stdexcept>

namespace commaview::video {
namespace {

constexpr size_t UDP_VIDEO_HEADER_BYTES = 60;

void append_u8(std::vector<uint8_t>& out, uint8_t value) {
  out.push_back(value);
}

void append_u16_be(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>(value & 0xff));
}

void append_u32_be(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>(value & 0xff));
}

void append_u64_be(std::vector<uint8_t>& out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
  }
}

uint8_t read_u8(const uint8_t* data, size_t* pos) {
  return data[(*pos)++];
}

uint16_t read_u16_be(const uint8_t* data, size_t* pos) {
  const uint16_t value = static_cast<uint16_t>((static_cast<uint16_t>(data[*pos]) << 8) |
                                               static_cast<uint16_t>(data[*pos + 1]));
  *pos += 2;
  return value;
}

uint32_t read_u32_be(const uint8_t* data, size_t* pos) {
  const uint32_t value = (static_cast<uint32_t>(data[*pos]) << 24) |
                         (static_cast<uint32_t>(data[*pos + 1]) << 16) |
                         (static_cast<uint32_t>(data[*pos + 2]) << 8) |
                         static_cast<uint32_t>(data[*pos + 3]);
  *pos += 4;
  return value;
}

uint64_t read_u64_be(const uint8_t* data, size_t* pos) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<uint64_t>(data[*pos + i]);
  }
  *pos += 8;
  return value;
}

bool is_valid_stream_id(uint8_t stream_id) {
  return stream_id == static_cast<uint8_t>(UdpVideoStreamId::Road) ||
         stream_id == static_cast<uint8_t>(UdpVideoStreamId::Wide) ||
         stream_id == static_cast<uint8_t>(UdpVideoStreamId::Driver);
}

bool flags_are_known(uint16_t flags) {
  return (flags & ~UDP_VIDEO_KNOWN_FLAGS) == 0;
}

void validate_udp_video_packet_for_encode(const UdpVideoPacket& packet) {
  if (!is_valid_stream_id(static_cast<uint8_t>(packet.stream_id))) {
    throw std::invalid_argument("UDP video packet has invalid stream id");
  }
  if (!flags_are_known(packet.flags)) {
    throw std::invalid_argument("UDP video packet has unknown flags");
  }
  if (packet.frame_packet_count == 0 || packet.frame_packet_index >= packet.frame_packet_count) {
    throw std::invalid_argument("UDP video packet frame packet index is out of range");
  }
  if (packet.payload.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("UDP video payload is too large to encode");
  }
  if (packet.payload.size() > packet.frame_byte_length ||
      packet.frame_byte_offset > packet.frame_byte_length - packet.payload.size()) {
    throw std::invalid_argument("UDP video packet frame byte range is out of bounds");
  }
  if (UDP_VIDEO_HEADER_BYTES + packet.payload.size() > UDP_VIDEO_MAX_DATAGRAM_BYTES) {
    throw std::length_error("UDP video datagram exceeds max datagram size");
  }
}

}  // namespace

std::vector<uint8_t> encode_udp_video_packet(const UdpVideoPacket& packet) {
  validate_udp_video_packet_for_encode(packet);

  std::vector<uint8_t> out;
  out.reserve(UDP_VIDEO_HEADER_BYTES + packet.payload.size());
  append_u32_be(out, UDP_VIDEO_MAGIC);
  append_u8(out, UDP_VIDEO_VERSION);
  append_u8(out, static_cast<uint8_t>(UdpVideoPacketType::Video));
  append_u8(out, static_cast<uint8_t>(packet.stream_id));
  append_u8(out, 0);  // reserved
  append_u16_be(out, packet.session_id);
  append_u16_be(out, packet.flags);
  append_u64_be(out, packet.packet_sequence);
  append_u64_be(out, packet.timestamp_nanos);
  append_u32_be(out, packet.frame_sequence);
  append_u16_be(out, packet.frame_packet_index);
  append_u16_be(out, packet.frame_packet_count);
  append_u32_be(out, packet.frame_byte_offset);
  append_u32_be(out, packet.frame_byte_length);
  append_u32_be(out, packet.codec_header_length);
  append_u32_be(out, packet.width);
  append_u32_be(out, packet.height);
  append_u32_be(out, static_cast<uint32_t>(packet.payload.size()));
  out.insert(out.end(), packet.payload.begin(), packet.payload.end());
  return out;
}

DecodeResult<UdpVideoPacket> decode_udp_video_packet(const uint8_t* data, size_t size) {
  if (size > UDP_VIDEO_MAX_DATAGRAM_BYTES) {
    return DecodeResult<UdpVideoPacket>::failure("UDP video datagram exceeds max payload size");
  }
  if (data == nullptr) {
    return DecodeResult<UdpVideoPacket>::failure("UDP video datagram data is null");
  }
  if (size < UDP_VIDEO_HEADER_BYTES) {
    return DecodeResult<UdpVideoPacket>::failure("UDP video datagram is too short");
  }

  size_t pos = 0;
  const uint32_t magic = read_u32_be(data, &pos);
  if (magic != UDP_VIDEO_MAGIC) {
    return DecodeResult<UdpVideoPacket>::failure("UDP video datagram has wrong magic");
  }

  const uint8_t version = read_u8(data, &pos);
  if (version != UDP_VIDEO_VERSION) {
    return DecodeResult<UdpVideoPacket>::failure("UDP video datagram has unsupported version");
  }

  const uint8_t packet_type = read_u8(data, &pos);
  if (packet_type != static_cast<uint8_t>(UdpVideoPacketType::Video)) {
    return DecodeResult<UdpVideoPacket>::failure("UDP video datagram has unsupported packet type");
  }

  const uint8_t stream_id = read_u8(data, &pos);
  if (!is_valid_stream_id(stream_id)) {
    return DecodeResult<UdpVideoPacket>::failure("UDP video datagram has invalid stream id");
  }

  const uint8_t reserved = read_u8(data, &pos);
  if (reserved != 0) {
    return DecodeResult<UdpVideoPacket>::failure("UDP video datagram has nonzero reserved byte");
  }

  UdpVideoPacket packet;
  packet.stream_id = static_cast<UdpVideoStreamId>(stream_id);
  packet.session_id = read_u16_be(data, &pos);
  packet.flags = read_u16_be(data, &pos);
  if (!flags_are_known(packet.flags)) {
    return DecodeResult<UdpVideoPacket>::failure("UDP video datagram has unknown flags");
  }
  packet.packet_sequence = read_u64_be(data, &pos);
  packet.timestamp_nanos = read_u64_be(data, &pos);
  packet.frame_sequence = read_u32_be(data, &pos);
  packet.frame_packet_index = read_u16_be(data, &pos);
  packet.frame_packet_count = read_u16_be(data, &pos);
  packet.frame_byte_offset = read_u32_be(data, &pos);
  packet.frame_byte_length = read_u32_be(data, &pos);
  packet.codec_header_length = read_u32_be(data, &pos);
  packet.width = read_u32_be(data, &pos);
  packet.height = read_u32_be(data, &pos);
  const uint32_t payload_length = read_u32_be(data, &pos);

  if (pos != UDP_VIDEO_HEADER_BYTES) {
    return DecodeResult<UdpVideoPacket>::failure("UDP video parser header size mismatch");
  }
  if (payload_length != size - UDP_VIDEO_HEADER_BYTES) {
    return DecodeResult<UdpVideoPacket>::failure("UDP video payload length mismatch");
  }
  if (packet.frame_packet_count == 0 || packet.frame_packet_index >= packet.frame_packet_count) {
    return DecodeResult<UdpVideoPacket>::failure("UDP video frame packet index is out of range");
  }
  if (payload_length > packet.frame_byte_length ||
      packet.frame_byte_offset > packet.frame_byte_length - payload_length) {
    return DecodeResult<UdpVideoPacket>::failure("UDP video frame byte range is out of bounds");
  }

  packet.payload.assign(data + pos, data + size);
  return DecodeResult<UdpVideoPacket>::success(std::move(packet));
}

}  // namespace commaview::video
