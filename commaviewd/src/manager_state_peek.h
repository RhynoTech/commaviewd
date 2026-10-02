#pragma once

// openpilot's process table for a support bundle: the newest managerState message, read from its
// msgq ring the way gps_peek.h reads GPS - mapped read-only, no subscriber, no reader slot - and
// only when a bundle is asked for.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace commaview::support {

struct ProcessState {
  std::string name;
  int32_t pid = 0;
  bool running = false;
  bool should_be_running = false;
  int32_t exit_code = 0;
};

struct ManagerStateSnapshot {
  uint64_t log_mono_ns = 0;  // when manager published it (CLOCK_MONOTONIC)
  std::vector<ProcessState> processes;
};

// The process table in one serialized cereal Event, or nothing when it isn't a managerState.
std::optional<ManagerStateSnapshot> manager_state_from_event(const uint8_t* data, size_t size);

// The snapshot as support JSON: processes that should run but don't, processes that exited with
// an error (a negative code is the signal that ended it; -9 is usually the OOM killer), and the
// whole table. now_mono_ns (CLOCK_MONOTONIC) dates it; 0 leaves the age out.
std::string manager_state_json(const ManagerStateSnapshot& snapshot, uint64_t now_mono_ns);

struct ManagerStatePeek {
  bool queue_exists = false;
  bool available = false;
  std::string json;  // always a JSON object; {"available":false,"reason":...} when unreadable
};

// Reads msgq_dir/msgq_managerState without subscribing.
ManagerStatePeek peek_manager_state(const std::string& msgq_dir);

}  // namespace commaview::support
