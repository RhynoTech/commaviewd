#pragma once

#include "udp_video_protocol.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace commaview::telemetry_snapshot {

// Lossy latest-wins overlay telemetry: every emit tick the bridge bundles the
// newest fresh ui-export payloads into one snapshot blob and streams it as
// CVUP TelemetrySnapshot datagrams. Snapshots are never repaired or
// retransmitted; a client that misses fragments simply waits ~50 ms for the
// next snapshot. Reliable data (alerts, onroadEvents, control/session/config)
// stays on TCP.

inline constexpr uint8_t kSnapshotBlobVersion = 1;
inline constexpr size_t kSnapshotHeaderBytes = 40;
inline constexpr size_t kSnapshotEntryHeaderBytes = 10;
inline constexpr uint64_t kFastTierIntervalMs = 0;     // every tick (<= 20 Hz)
inline constexpr uint64_t kSlowTierIntervalMs = 100;   // <= 10 Hz

enum class SnapshotTier : uint8_t {
  Excluded = 0,  // reliable/event data: never carried in snapshots
  Fast = 1,      // render-critical: every emit tick
  Slow = 2,      // HUD/status: at most 10 Hz
};

// Tier per kTelemetryServices index. onroadEvents is event data whose loss is
// unacceptable, so it is excluded and stays TCP-only.
SnapshotTier snapshot_tier_for_service(uint8_t service_index);

struct SnapshotInput {
  uint8_t service_index = 0;
  uint64_t updated_at_ms = 0;
  const std::vector<uint8_t>* payload = nullptr;
};

// Tracks per-service inclusion state so unchanged frames are not resent and
// slow-tier services are rate limited. One builder per snapshot client stream.
class TelemetrySnapshotBuilder {
 public:
  // Returns the serialized snapshot blob, or an empty vector when no service
  // is due this tick. Inputs must carry fresh ui-export frames only.
  std::vector<uint8_t> build(const std::vector<SnapshotInput>& inputs, uint64_t now_ms);

  void reset();

 private:
  struct ServiceState {
    uint64_t last_included_updated_ms = 0;
    uint64_t last_included_wall_ms = 0;
  };
  ServiceState states_[20] = {};
};

// Splits a snapshot blob into CVUP TelemetrySnapshot datagrams.
//
// Datagram layout (big-endian):
//   0-9   common CVUP header (magic, version, type=7, stream=4, reserved,
//         session_id)
//   10-17 snapshot_sequence
//   18-25 snapshot_mono_ns
//   26-27 fragment_index
//   28-29 fragment_count
//   30-33 blob_byte_offset
//   34-37 blob_byte_length (total blob size)
//   38-39 payload_length
//   40+   payload (contiguous blob slice)
std::vector<std::vector<uint8_t>> packetize_telemetry_snapshot(
    const std::vector<uint8_t>& blob,
    uint16_t session_id,
    uint64_t snapshot_sequence,
    uint64_t snapshot_mono_ns,
    size_t target_payload_bytes = commaview::video::UDP_VIDEO_TARGET_PAYLOAD_BYTES);

}  // namespace commaview::telemetry_snapshot
