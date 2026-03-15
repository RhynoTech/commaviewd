#pragma once

#include <cstdint>
#include <vector>

namespace commaview::telemetry {

struct ServiceCounters {
  uint64_t ingest = 0;
  uint64_t coalesced = 0;
  uint64_t emittedJson = 0;
  uint64_t emittedRaw = 0;
  uint64_t dropped = 0;
};

struct LoopCounters {
  uint64_t iterations = 0;
  uint64_t totalMicros = 0;
  uint64_t maxMicros = 0;
  uint64_t overBudget = 0;
};

void note_ingest(std::vector<ServiceCounters>& counters, int idx, uint64_t count = 1);
void note_coalesced(std::vector<ServiceCounters>& counters, int idx, uint64_t count = 1);
void note_emit_json(std::vector<ServiceCounters>& counters, int idx, uint64_t count = 1);
void note_emit_raw(std::vector<ServiceCounters>& counters, int idx, uint64_t count = 1);
void note_drop(std::vector<ServiceCounters>& counters, int idx, uint64_t count = 1);

void note_loop(LoopCounters& loop, uint64_t elapsed_micros, int emit_ms_budget);
uint64_t average_loop_micros(const LoopCounters& loop);

bool service_counter_invariants_hold(const std::vector<ServiceCounters>& counters);

}  // namespace commaview::telemetry
