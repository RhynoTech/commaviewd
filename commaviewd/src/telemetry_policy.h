#pragma once

#include "framing.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace commaview::telemetry {

enum class ServiceMode { Off, Sample, Pass };

struct ServicePolicy {
  ServiceMode mode = ServiceMode::Off;
  int sample_hz = 0;
};

struct NamedServicePolicy {
  const char* service = nullptr;
  ServicePolicy policy = {};
};

inline constexpr NamedServicePolicy kDefaultServicePolicies[] = {
  {"uiStateOnroad", {ServiceMode::Pass, 0}},
  {"selfdriveState", {ServiceMode::Pass, 0}},
  {"carState", {ServiceMode::Pass, 0}},
  {"controlsState", {ServiceMode::Pass, 0}},
  {"onroadEvents", {ServiceMode::Pass, 0}},
  {"driverMonitoringState", {ServiceMode::Pass, 0}},
  {"driverStateV2", {ServiceMode::Pass, 0}},
  {"modelV2", {ServiceMode::Pass, 0}},
  {"radarState", {ServiceMode::Pass, 0}},
  {"liveCalibration", {ServiceMode::Pass, 0}},
  {"carOutput", {ServiceMode::Pass, 0}},
  {"carControl", {ServiceMode::Pass, 0}},
  {"liveParameters", {ServiceMode::Pass, 0}},
  {"longitudinalPlan", {ServiceMode::Pass, 0}},
  {"carParams", {ServiceMode::Pass, 0}},
  {"deviceState", {ServiceMode::Pass, 0}},
  {"roadCameraState", {ServiceMode::Pass, 0}},
  {"pandaStatesSummary", {ServiceMode::Pass, 0}},
  {"onroadProjection", {ServiceMode::Pass, 0}},
  {"wideRoadCameraState", {ServiceMode::Pass, 0}},
};

inline constexpr size_t kDefaultServicePolicyCount = sizeof(kDefaultServicePolicies) / sizeof(kDefaultServicePolicies[0]);

inline ServicePolicy default_service_policy_for_name(const char* service_name) {
  if (service_name == nullptr) return {};
  for (size_t i = 0; i < kDefaultServicePolicyCount; ++i) {
    if (std::strcmp(kDefaultServicePolicies[i].service, service_name) == 0) {
      return kDefaultServicePolicies[i].policy;
    }
  }
  return {};
}

inline bool service_policy_subscribes(const ServicePolicy& policy) {
  return policy.mode != ServiceMode::Off;
}

inline bool service_policy_samples(const ServicePolicy& policy) {
  return policy.mode == ServiceMode::Sample;
}

inline bool telemetry_policy_fetches_latest(const ServicePolicy& policy) {
  return service_policy_subscribes(policy);
}

inline bool telemetry_policy_allows_emit(const ServicePolicy& policy,
                                         uint64_t frame_updated_ms,
                                         uint64_t now_ms,
                                         uint64_t last_frame_updated_ms,
                                         uint64_t last_emit_wall_ms) {
  if (!service_policy_subscribes(policy)) return false;
  if (frame_updated_ms <= last_frame_updated_ms) return false;
  if (!service_policy_samples(policy)) return true;
  if (policy.sample_hz <= 0) return false;
  const uint64_t interval_ms = static_cast<uint64_t>(1000 / policy.sample_hz);
  const uint64_t min_interval_ms = interval_ms == 0 ? 1 : interval_ms;
  return last_emit_wall_ms == 0 || now_ms >= last_emit_wall_ms + min_interval_ms;
}

inline bool telemetry_send_failure_is_droppable(const commaview::net::SendResult& result) {
  return result.status == commaview::net::SendStatus::Backpressure && result.bytes_sent == 0;
}

// TCP keepalive rate for services that the client is receiving via the lossy
// UDP snapshot path instead.
inline constexpr int kUdpSnapshotTcpKeepaliveHz = 2;

// Alert-bearing services must stay full-rate on the reliable TCP path even
// when the client receives overlay telemetry via UDP snapshots: a lost alert
// is unacceptable, and alerts ride selfdriveState/controlsState/onroadEvents.
inline bool service_keeps_full_tcp_rate_for_udp_snapshot(const char* service_name) {
  if (service_name == nullptr) return true;
  return std::strcmp(service_name, "selfdriveState") == 0 ||
         std::strcmp(service_name, "controlsState") == 0 ||
         std::strcmp(service_name, "onroadEvents") == 0;
}

// When the client opted into UDP snapshot transport, demote Pass-mode services
// that ride the snapshot path to a low TCP keepalive rate. Off/Sample policies
// and alert-bearing services are returned unchanged.
inline ServicePolicy demote_policy_for_udp_snapshot(const ServicePolicy& policy,
                                                    const char* service_name) {
  if (policy.mode != ServiceMode::Pass) return policy;
  if (service_keeps_full_tcp_rate_for_udp_snapshot(service_name)) return policy;
  return {ServiceMode::Sample, kUdpSnapshotTcpKeepaliveHz};
}

}  // namespace commaview::telemetry
