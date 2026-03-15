#include "telemetry_stats.h"

#include <cassert>
#include <vector>

using commaview::telemetry::LoopCounters;
using commaview::telemetry::ServiceCounters;

static void test_service_counters_and_invariants() {
  std::vector<ServiceCounters> counters(3);

  commaview::telemetry::note_ingest(counters, 1);
  commaview::telemetry::note_ingest(counters, 1, 2);
  commaview::telemetry::note_coalesced(counters, 1, 2);
  commaview::telemetry::note_emit_raw(counters, 1, 1);
  commaview::telemetry::note_emit_json(counters, 1, 1);
  commaview::telemetry::note_drop(counters, 1, 1);

  assert(counters[1].ingest == 3);
  assert(counters[1].coalesced == 2);
  assert(counters[1].emittedRaw == 1);
  assert(counters[1].emittedJson == 1);
  assert(counters[1].dropped == 1);
  assert(commaview::telemetry::service_counter_invariants_hold(counters));

  counters[2].coalesced = 1;
  counters[2].ingest = 0;
  assert(!commaview::telemetry::service_counter_invariants_hold(counters));
}

static void test_loop_counters() {
  LoopCounters loop{};
  commaview::telemetry::note_loop(loop, 400, 10);
  commaview::telemetry::note_loop(loop, 15000, 10);

  assert(loop.iterations == 2);
  assert(loop.totalMicros == 15400);
  assert(loop.maxMicros == 15000);
  assert(loop.overBudget == 1);
  assert(commaview::telemetry::average_loop_micros(loop) == 7700);
}

int main() {
  test_service_counters_and_invariants();
  test_loop_counters();
  return 0;
}
