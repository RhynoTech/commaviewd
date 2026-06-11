#pragma once

#include "udp_video_protocol.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace commaview::video {

struct UdpVideoFrameForPacketizing {
  UdpVideoStreamId stream_id = UdpVideoStreamId::Road;
  uint16_t session_id = 0;
  uint32_t frame_sequence = 0;
  uint64_t timestamp_nanos = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool is_keyframe = false;
  std::vector<uint8_t> codec_header;
  std::vector<uint8_t> data;
};

std::vector<UdpVideoPacket> packetize_udp_video_frame(
    const UdpVideoFrameForPacketizing& frame,
    uint64_t* next_packet_sequence,
    size_t target_payload_bytes = UDP_VIDEO_TARGET_PAYLOAD_BYTES);

}  // namespace commaview::video
