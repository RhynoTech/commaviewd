#include "udp_telemetry_snapshot.h"

#include <algorithm>
#include <cstring>

namespace commaview::telemetry_snapshot {

namespace {

void append_u8(std::vector<uint8_t>& out, uint8_t value) { out.push_back(value); }

void append_u16_be(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void append_u32_be(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void append_u64_be(std::vector<uint8_t>& out, uint64_t value) {
  append_u32_be(out, static_cast<uint32_t>(value >> 32));
  append_u32_be(out, static_cast<uint32_t>(value));
}

}  // namespace

SnapshotTier snapshot_tier_for_service(uint8_t service_index) {
  switch (service_index) {
    // Render-critical at video cadence: path/lanes/edges, leads, projection,
    // speed/cruise, engagement + curvature/torque, camera selection, driver
    // monitoring pose, HUD visibility flags, calibration.
    case 0:   // uiStateOnroad
    case 1:   // selfdriveState (also stays full-rate on TCP for alerts)
    case 2:   // carState
    case 3:   // controlsState (also stays full-rate on TCP for alerts)
    case 5:   // driverMonitoringState
    case 6:   // driverStateV2
    case 7:   // modelV2
    case 8:   // radarState
    case 9:   // liveCalibration
    case 11:  // carControl
    case 13:  // longitudinalPlan
    case 18:  // onroadProjection
      return SnapshotTier::Fast;
    // Slow-changing HUD/status data.
    case 10:  // carOutput
    case 12:  // liveParameters
    case 14:  // carParams
    case 15:  // deviceState
    case 16:  // roadCameraState
    case 17:  // pandaStatesSummary
    case 19:  // wideRoadCameraState
      return SnapshotTier::Slow;
    // onroadEvents (4) is event data; loss is unacceptable so it never rides
    // the lossy snapshot path.
    default:
      return SnapshotTier::Excluded;
  }
}

std::vector<uint8_t> TelemetrySnapshotBuilder::build(const std::vector<SnapshotInput>& inputs,
                                                     uint64_t now_ms) {
  std::vector<const SnapshotInput*> due;
  due.reserve(inputs.size());
  for (const SnapshotInput& input : inputs) {
    if (input.payload == nullptr || input.payload->empty()) continue;
    if (input.service_index >= 20) continue;
    const SnapshotTier tier = snapshot_tier_for_service(input.service_index);
    if (tier == SnapshotTier::Excluded) continue;

    ServiceState& state = states_[input.service_index];
    if (input.updated_at_ms <= state.last_included_updated_ms) continue;
    const uint64_t min_interval_ms =
        tier == SnapshotTier::Slow ? kSlowTierIntervalMs : kFastTierIntervalMs;
    if (min_interval_ms > 0 && state.last_included_wall_ms != 0 &&
        now_ms < state.last_included_wall_ms + min_interval_ms) {
      continue;
    }
    due.push_back(&input);
  }

  if (due.empty()) {
    staged_.clear();
    return {};
  }

  std::vector<uint8_t> blob;
  blob.reserve(64);
  append_u8(blob, kSnapshotBlobVersion);
  append_u8(blob, static_cast<uint8_t>(due.size()));
  staged_.clear();
  staged_.reserve(due.size());
  staged_wall_ms_ = now_ms;
  for (const SnapshotInput* input : due) {
    staged_.push_back({input->service_index, input->updated_at_ms});

    const uint64_t age = now_ms > input->updated_at_ms ? now_ms - input->updated_at_ms : 0;
    append_u8(blob, input->service_index);
    append_u8(blob, 0);  // flags (reserved)
    append_u32_be(blob, static_cast<uint32_t>(std::min<uint64_t>(age, UINT32_MAX)));
    append_u32_be(blob, static_cast<uint32_t>(input->payload->size()));
    blob.insert(blob.end(), input->payload->begin(), input->payload->end());
  }
  return blob;
}

void TelemetrySnapshotBuilder::commit_last_build() {
  for (const StagedInclusion& staged : staged_) {
    ServiceState& state = states_[staged.service_index];
    state.last_included_updated_ms = staged.updated_at_ms;
    state.last_included_wall_ms = staged_wall_ms_;
  }
  staged_.clear();
}

void TelemetrySnapshotBuilder::reset() {
  for (ServiceState& state : states_) {
    state = ServiceState{};
  }
  staged_.clear();
}

std::vector<std::vector<uint8_t>> packetize_telemetry_snapshot(const std::vector<uint8_t>& blob,
                                                               uint16_t session_id,
                                                               uint64_t snapshot_sequence,
                                                               uint64_t snapshot_mono_ns,
                                                               size_t target_payload_bytes) {
  std::vector<std::vector<uint8_t>> datagrams;
  if (blob.empty() || target_payload_bytes == 0) return datagrams;

  const size_t max_payload =
      std::min(target_payload_bytes,
               commaview::video::UDP_VIDEO_MAX_DATAGRAM_BYTES - kSnapshotHeaderBytes);
  const size_t fragment_count = (blob.size() + max_payload - 1) / max_payload;
  if (fragment_count > UINT16_MAX) return datagrams;

  datagrams.reserve(fragment_count);
  size_t offset = 0;
  for (size_t index = 0; index < fragment_count; ++index) {
    const size_t slice = std::min(max_payload, blob.size() - offset);
    std::vector<uint8_t> datagram;
    datagram.reserve(kSnapshotHeaderBytes + slice);
    append_u32_be(datagram, commaview::video::UDP_VIDEO_MAGIC);
    append_u8(datagram, commaview::video::UDP_VIDEO_VERSION);
    append_u8(datagram, static_cast<uint8_t>(commaview::video::UdpVideoPacketType::TelemetrySnapshot));
    append_u8(datagram, static_cast<uint8_t>(commaview::video::UdpVideoStreamId::Telemetry));
    append_u8(datagram, 0);  // reserved
    append_u16_be(datagram, session_id);
    append_u64_be(datagram, snapshot_sequence);
    append_u64_be(datagram, snapshot_mono_ns);
    append_u16_be(datagram, static_cast<uint16_t>(index));
    append_u16_be(datagram, static_cast<uint16_t>(fragment_count));
    append_u32_be(datagram, static_cast<uint32_t>(offset));
    append_u32_be(datagram, static_cast<uint32_t>(blob.size()));
    append_u16_be(datagram, static_cast<uint16_t>(slice));
    datagram.insert(datagram.end(), blob.begin() + offset, blob.begin() + offset + slice);
    datagrams.push_back(std::move(datagram));
    offset += slice;
  }
  return datagrams;
}

}  // namespace commaview::telemetry_snapshot
