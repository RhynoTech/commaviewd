#include "telemetry_stats.h"

#include <algorithm>

namespace commaview::telemetry {

namespace {

inline bool valid_index(const std::vector<ServiceCounters>& counters, int idx) {
  return idx >= 0 && static_cast<size_t>(idx) < counters.size();
}

}  // namespace

void note_ingest(std::vector<ServiceCounters>& counters, int idx, uint64_t count) {
  if (!valid_index(counters, idx)) return;
  counters[idx].ingest += count;
}

void note_coalesced(std::vector<ServiceCounters>& counters, int idx, uint64_t count) {
  if (!valid_index(counters, idx)) return;
  counters[idx].coalesced += count;
}

void note_emit_json(std::vector<ServiceCounters>& counters, int idx, uint64_t count) {
  if (!valid_index(counters, idx)) return;
  counters[idx].emittedJson += count;
}

void note_emit_raw(std::vector<ServiceCounters>& counters, int idx, uint64_t count) {
  if (!valid_index(counters, idx)) return;
  counters[idx].emittedRaw += count;
}

void note_drop(std::vector<ServiceCounters>& counters, int idx, uint64_t count) {
  if (!valid_index(counters, idx)) return;
  counters[idx].dropped += count;
}

void note_loop(LoopCounters& loop, uint64_t elapsed_micros, int emit_ms_budget) {
  loop.iterations += 1;
  loop.totalMicros += elapsed_micros;
  loop.maxMicros = std::max(loop.maxMicros, elapsed_micros);

  const uint64_t budget_micros = emit_ms_budget > 0 ? static_cast<uint64_t>(emit_ms_budget) * 1000ULL : 0ULL;
  if (budget_micros > 0 && elapsed_micros > budget_micros) {
    loop.overBudget += 1;
  }
}

uint64_t average_loop_micros(const LoopCounters& loop) {
  if (loop.iterations == 0) return 0;
  return loop.totalMicros / loop.iterations;
}

bool service_counter_invariants_hold(const std::vector<ServiceCounters>& counters) {
  for (const auto& c : counters) {
    if (c.coalesced > c.ingest) return false;
  }
  return true;
}

}  // namespace commaview::telemetry
