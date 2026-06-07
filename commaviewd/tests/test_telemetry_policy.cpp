#include "telemetry_policy.h"

#include "framing.h"

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

static void test_zero_byte_telemetry_backpressure_is_droppable() {
  commaview::net::SendResult result{};
  result.status = commaview::net::SendStatus::Backpressure;
  result.bytes_sent = 0;
  assert(commaview::telemetry::telemetry_send_failure_is_droppable(result));
}

static void test_partial_telemetry_backpressure_is_fatal() {
  commaview::net::SendResult result{};
  result.status = commaview::net::SendStatus::Backpressure;
  result.bytes_sent = 1;
  assert(!commaview::telemetry::telemetry_send_failure_is_droppable(result));
}

static void test_disconnected_telemetry_send_is_fatal() {
  commaview::net::SendResult result{};
  result.status = commaview::net::SendStatus::Disconnected;
  result.bytes_sent = 0;
  assert(!commaview::telemetry::telemetry_send_failure_is_droppable(result));
}

int main() {
  test_off_policy_does_not_fetch_or_emit();
  test_pass_policy_emits_new_frame_once();
  test_sample_policy_obeys_rate();
  test_zero_byte_telemetry_backpressure_is_droppable();
  test_partial_telemetry_backpressure_is_fatal();
  test_disconnected_telemetry_send_is_fatal();
  return 0;
}
