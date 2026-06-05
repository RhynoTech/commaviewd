#include "telemetry_policy.h"

#include <cassert>

using commaview::telemetry::ServiceMode;
using commaview::telemetry::ServicePolicy;

static void test_off_policy_does_not_fetch_or_emit() {
  ServicePolicy policy{ServiceMode::Off, 0};
  assert(!commaview::telemetry::telemetry_policy_fetches_latest(policy));
  assert(!commaview::telemetry::telemetry_policy_allows_emit(policy, 1000, 1000, 0, 0));
}

static void test_pass_policy_emits_new_frame_once() {
  ServicePolicy policy{ServiceMode::Pass, 0};
  assert(commaview::telemetry::telemetry_policy_fetches_latest(policy));
  assert(commaview::telemetry::telemetry_policy_allows_emit(policy, 1000, 1100, 999, 0));
  assert(!commaview::telemetry::telemetry_policy_allows_emit(policy, 1000, 1200, 1000, 0));
}

static void test_sample_policy_obeys_rate() {
  ServicePolicy policy{ServiceMode::Sample, 2};
  assert(commaview::telemetry::telemetry_policy_fetches_latest(policy));
  assert(commaview::telemetry::telemetry_policy_allows_emit(policy, 1000, 1000, 0, 0));
  assert(!commaview::telemetry::telemetry_policy_allows_emit(policy, 1100, 1200, 1000, 1000));
  assert(commaview::telemetry::telemetry_policy_allows_emit(policy, 1200, 1500, 1000, 1000));
}

int main() {
  test_off_policy_does_not_fetch_or_emit();
  test_pass_policy_emits_new_frame_once();
  test_sample_policy_obeys_rate();
  return 0;
}
