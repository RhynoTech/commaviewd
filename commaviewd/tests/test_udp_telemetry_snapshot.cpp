#include "udp_telemetry_snapshot.h"

#include "telemetry_policy.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using commaview::telemetry_snapshot::SnapshotInput;
using commaview::telemetry_snapshot::SnapshotTier;
using commaview::telemetry_snapshot::TelemetrySnapshotBuilder;
using commaview::telemetry_snapshot::kSnapshotBlobVersion;
using commaview::telemetry_snapshot::kSnapshotHeaderBytes;
using commaview::telemetry_snapshot::packetize_telemetry_snapshot;
using commaview::telemetry_snapshot::snapshot_tier_for_service;

namespace {

uint16_t read_u16(const std::vector<uint8_t>& data, size_t pos) {
  return static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
}

uint32_t read_u32(const std::vector<uint8_t>& data, size_t pos) {
  return (static_cast<uint32_t>(data[pos]) << 24) | (static_cast<uint32_t>(data[pos + 1]) << 16) |
         (static_cast<uint32_t>(data[pos + 2]) << 8) | static_cast<uint32_t>(data[pos + 3]);
}

uint64_t read_u64(const std::vector<uint8_t>& data, size_t pos) {
  return (static_cast<uint64_t>(read_u32(data, pos)) << 32) | read_u32(data, pos + 4);
}

std::vector<uint8_t> payload_of(const std::string& text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

SnapshotInput input_for(uint8_t service_index, uint64_t updated_at_ms, const std::vector<uint8_t>* payload) {
  SnapshotInput input;
  input.service_index = service_index;
  input.updated_at_ms = updated_at_ms;
  input.payload = payload;
  return input;
}

}  // namespace

static void test_tier_table_covers_all_services() {
  // Render-critical services ride the fast tier at the emit cadence.
  for (uint8_t index : {0, 1, 2, 3, 5, 6, 7, 8, 9, 11, 13, 18}) {
    assert(snapshot_tier_for_service(index) == SnapshotTier::Fast);
  }
  // Slow-changing HUD/status data is rate limited.
  for (uint8_t index : {10, 12, 14, 15, 16, 17, 19}) {
    assert(snapshot_tier_for_service(index) == SnapshotTier::Slow);
  }
  // onroadEvents is event data: reliable TCP only.
  assert(snapshot_tier_for_service(4) == SnapshotTier::Excluded);
  assert(snapshot_tier_for_service(20) == SnapshotTier::Excluded);
}

static void test_builder_serializes_due_entries() {
  TelemetrySnapshotBuilder builder;
  const auto model = payload_of("{\"m\":1}");
  const auto radar = payload_of("{\"r\":2}");
  const std::vector<SnapshotInput> inputs = {
      input_for(7, 1000, &model),
      input_for(8, 990, &radar),
  };

  const auto blob = builder.build(inputs, 1010);

  assert(!blob.empty());
  assert(blob[0] == kSnapshotBlobVersion);
  assert(blob[1] == 2);
  size_t pos = 2;
  assert(blob[pos] == 7);
  assert(blob[pos + 1] == 0);
  assert(read_u32(blob, pos + 2) == 10);  // age_ms = 1010 - 1000
  const uint32_t model_len = read_u32(blob, pos + 6);
  assert(model_len == model.size());
  assert(std::memcmp(blob.data() + pos + 10, model.data(), model.size()) == 0);
  pos += 10 + model_len;
  assert(blob[pos] == 8);
  assert(read_u32(blob, pos + 2) == 20);  // age_ms = 1010 - 990
  assert(read_u32(blob, pos + 6) == radar.size());
}

static void test_builder_skips_unchanged_and_excluded_services() {
  TelemetrySnapshotBuilder builder;
  const auto model = payload_of("{\"m\":1}");
  const auto events = payload_of("{\"e\":1}");
  const std::vector<SnapshotInput> inputs = {
      input_for(7, 1000, &model),
      input_for(4, 1000, &events),  // onroadEvents: excluded
  };

  const auto first = builder.build(inputs, 1000);
  assert(!first.empty());
  assert(first[1] == 1);  // only modelV2
  builder.commit_last_build();

  // Unchanged updated_at_ms: nothing due, no snapshot.
  const auto second = builder.build(inputs, 1050);
  assert(second.empty());

  // New frame is included again.
  const std::vector<SnapshotInput> updated = {input_for(7, 1050, &model)};
  const auto third = builder.build(updated, 1060);
  assert(!third.empty());
  assert(third[1] == 1);
}

static void test_builder_rate_limits_slow_tier() {
  TelemetrySnapshotBuilder builder;
  const auto device = payload_of("{\"d\":1}");

  const auto first = builder.build({input_for(15, 1000, &device)}, 1000);
  assert(!first.empty());
  builder.commit_last_build();

  // Fresh update 50 ms later is held back by the 10 Hz slow tier.
  const auto second = builder.build({input_for(15, 1050, &device)}, 1050);
  assert(second.empty());

  // After the 100 ms interval it flows again.
  const auto third = builder.build({input_for(15, 1050, &device)}, 1100);
  assert(!third.empty());
}

static void test_uncommitted_build_is_retried_after_a_dropped_send() {
  TelemetrySnapshotBuilder builder;
  const auto model = payload_of("{\"m\":1}");
  const auto device = payload_of("{\"d\":1}");

  // First snapshot is built but never committed (send failed/dropped).
  const auto dropped = builder.build({input_for(7, 1000, &model), input_for(15, 1000, &device)}, 1000);
  assert(!dropped.empty());
  assert(dropped[1] == 2);

  // The same unchanged state stays due on the next tick.
  const auto retried = builder.build({input_for(7, 1000, &model), input_for(15, 1000, &device)}, 1050);
  assert(!retried.empty());
  assert(retried[1] == 2);
  builder.commit_last_build();

  // Once committed, unchanged state is suppressed again.
  assert(builder.build({input_for(7, 1000, &model), input_for(15, 1000, &device)}, 1200).empty());
}

static void test_builder_reset_resends_state() {
  TelemetrySnapshotBuilder builder;
  const auto model = payload_of("{\"m\":1}");
  assert(!builder.build({input_for(7, 1000, &model)}, 1000).empty());
  builder.commit_last_build();
  assert(builder.build({input_for(7, 1000, &model)}, 1050).empty());

  builder.reset();
  assert(!builder.build({input_for(7, 1000, &model)}, 1100).empty());
}

static void test_packetizer_single_fragment_round_trip() {
  const std::vector<uint8_t> blob = payload_of("snapshot-blob");
  const auto datagrams = packetize_telemetry_snapshot(blob, 0xBEEF, 42, 99'000'000ULL);

  assert(datagrams.size() == 1);
  const auto& d = datagrams[0];
  assert(d.size() == kSnapshotHeaderBytes + blob.size());
  assert(read_u32(d, 0) == commaview::video::UDP_VIDEO_MAGIC);
  assert(d[4] == commaview::video::UDP_VIDEO_VERSION);
  assert(d[5] == static_cast<uint8_t>(commaview::video::UdpVideoPacketType::TelemetrySnapshot));
  assert(d[6] == static_cast<uint8_t>(commaview::video::UdpVideoStreamId::Telemetry));
  assert(d[7] == 0);
  assert(read_u16(d, 8) == 0xBEEF);
  assert(read_u64(d, 10) == 42);
  assert(read_u64(d, 18) == 99'000'000ULL);
  assert(read_u16(d, 26) == 0);  // fragment_index
  assert(read_u16(d, 28) == 1);  // fragment_count
  assert(read_u32(d, 30) == 0);  // blob_byte_offset
  assert(read_u32(d, 34) == blob.size());
  assert(read_u16(d, 38) == blob.size());
  assert(std::memcmp(d.data() + kSnapshotHeaderBytes, blob.data(), blob.size()) == 0);
}

static void test_packetizer_splits_and_reassembles_large_blobs() {
  std::vector<uint8_t> blob(3000);
  for (size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<uint8_t>(i & 0xff);

  const size_t target_payload = 1200;
  const auto datagrams = packetize_telemetry_snapshot(blob, 7, 9, 1, target_payload);

  assert(datagrams.size() == 3);
  std::vector<uint8_t> reassembled(blob.size());
  size_t covered = 0;
  for (size_t i = 0; i < datagrams.size(); ++i) {
    const auto& d = datagrams[i];
    assert(d.size() <= commaview::video::UDP_VIDEO_MAX_DATAGRAM_BYTES);
    assert(read_u16(d, 26) == i);
    assert(read_u16(d, 28) == datagrams.size());
    assert(read_u32(d, 34) == blob.size());
    const uint32_t offset = read_u32(d, 30);
    const uint16_t len = read_u16(d, 38);
    assert(d.size() == kSnapshotHeaderBytes + len);
    std::memcpy(reassembled.data() + offset, d.data() + kSnapshotHeaderBytes, len);
    covered += len;
  }
  assert(covered == blob.size());
  assert(reassembled == blob);
}

static void test_packetizer_rejects_empty_blob() {
  assert(packetize_telemetry_snapshot({}, 1, 1, 1).empty());
}

static void test_tcp_demotion_keeps_alert_services_full_rate() {
  using commaview::telemetry::ServiceMode;
  using commaview::telemetry::ServicePolicy;
  const ServicePolicy pass{ServiceMode::Pass, 0};

  for (const char* service : {"selfdriveState", "controlsState", "onroadEvents"}) {
    const auto kept = commaview::telemetry::demote_policy_for_udp_snapshot(pass, service);
    assert(kept.mode == ServiceMode::Pass);
  }

  const auto demoted = commaview::telemetry::demote_policy_for_udp_snapshot(pass, "modelV2");
  assert(demoted.mode == ServiceMode::Sample);
  assert(demoted.sample_hz == commaview::telemetry::kUdpSnapshotTcpKeepaliveHz);

  // Off and Sample configurations are respected unchanged.
  const ServicePolicy off{ServiceMode::Off, 0};
  assert(commaview::telemetry::demote_policy_for_udp_snapshot(off, "modelV2").mode == ServiceMode::Off);
  const ServicePolicy sampled{ServiceMode::Sample, 5};
  const auto sampled_kept = commaview::telemetry::demote_policy_for_udp_snapshot(sampled, "modelV2");
  assert(sampled_kept.mode == ServiceMode::Sample);
  assert(sampled_kept.sample_hz == 5);
}

int main() {
  test_tier_table_covers_all_services();
  test_builder_serializes_due_entries();
  test_builder_skips_unchanged_and_excluded_services();
  test_builder_rate_limits_slow_tier();
  test_uncommitted_build_is_retried_after_a_dropped_send();
  test_builder_reset_resends_state();
  test_packetizer_single_fragment_round_trip();
  test_packetizer_splits_and_reassembles_large_blobs();
  test_packetizer_rejects_empty_blob();
  test_tcp_demotion_keeps_alert_services_full_rate();
  printf("PASS: udp telemetry snapshot tests passed\n");
  return 0;
}
