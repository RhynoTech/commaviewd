#pragma once

#include <cstddef>
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
  {"commaViewControl", {ServiceMode::Pass, 0}},
  {"commaViewScene", {ServiceMode::Pass, 0}},
  {"commaViewStatus", {ServiceMode::Pass, 0}},
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

}  // namespace commaview::telemetry
