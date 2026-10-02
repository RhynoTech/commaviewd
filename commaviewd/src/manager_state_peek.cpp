#include "manager_state_peek.h"

#include <capnp/serialize.h>
#include <kj/array.h>
#include <sys/stat.h>
#include <time.h>

#include <cstring>
#include <sstream>

#include "cereal/gen/cpp/log.capnp.h"
#include "gps_peek.h"

namespace commaview::support {
namespace {

// openpilot runs a few dozen processes; anything past this isn't a process table.
constexpr size_t kMaxProcesses = 256;
constexpr size_t kMaxNameBytes = 64;

std::string json_string(const std::string& in) {
  std::string out = "\"";
  for (char c : in) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (uc < 0x20 || uc == 0x7f) {
      out += ' ';
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

// manager is Python: its messages are stamped with time.monotonic().
uint64_t monotonic_ns() {
  timespec ts {};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

std::string unavailable_json(const std::string& queue_path, const std::string& reason) {
  return "{\"available\":false,\"source\":" + json_string(queue_path) + ",\"reason\":" + json_string(reason) + "}";
}

void process_json(std::ostringstream& out, const ProcessState& p, bool full) {
  out << "{\"name\":" << json_string(p.name);
  if (full) {
    out << ",\"running\":" << (p.running ? "true" : "false")
        << ",\"shouldBeRunning\":" << (p.should_be_running ? "true" : "false");
  }
  out << ",\"pid\":" << p.pid << ",\"exitCode\":" << p.exit_code;
  if (p.exit_code < 0) out << ",\"exitSignal\":" << -static_cast<int64_t>(p.exit_code);
  out << "}";
}

}  // namespace

std::optional<ManagerStateSnapshot> manager_state_from_event(const uint8_t* data, size_t size) {
  if (data == nullptr || size < sizeof(capnp::word) || size % sizeof(capnp::word) != 0) return std::nullopt;
  kj::Array<capnp::word> words = kj::heapArray<capnp::word>(size / sizeof(capnp::word));
  std::memcpy(words.begin(), data, size);
  try {
    capnp::ReaderOptions options;
    options.traversalLimitInWords = 1 << 16;
    capnp::FlatArrayMessageReader reader(words, options);
    auto event = reader.getRoot<cereal::Event>();
    if (!event.isManagerState()) return std::nullopt;
    ManagerStateSnapshot snapshot;
    snapshot.log_mono_ns = event.getLogMonoTime();
    auto processes = event.getManagerState().getProcesses();
    for (auto p : processes) {
      if (snapshot.processes.size() >= kMaxProcesses) break;
      ProcessState state;
      state.name = std::string(p.getName().cStr()).substr(0, kMaxNameBytes);
      state.pid = p.getPid();
      state.running = p.getRunning();
      state.should_be_running = p.getShouldBeRunning();
      state.exit_code = p.getExitCode();
      snapshot.processes.push_back(std::move(state));
    }
    return snapshot;
  } catch (...) {
    return std::nullopt;
  }
}

std::string manager_state_json(const ManagerStateSnapshot& snapshot, uint64_t now_mono_ns) {
  std::ostringstream out;
  out << "{\"available\":true,\"source\":\"managerState\"";
  if (now_mono_ns > 0 && snapshot.log_mono_ns > 0 && now_mono_ns >= snapshot.log_mono_ns) {
    // managerState comes about twice a second; an old one means manager itself has stopped.
    out << ",\"ageMs\":" << (now_mono_ns - snapshot.log_mono_ns) / 1000000ULL;
  }
  out << ",\"processCount\":" << snapshot.processes.size();
  out << ",\"shouldRunButNotRunning\":[";
  bool first = true;
  for (const auto& p : snapshot.processes) {
    if (!p.should_be_running || p.running) continue;
    if (!first) out << ",";
    first = false;
    process_json(out, p, false);
  }
  out << "],\"exitedWithError\":[";
  first = true;
  for (const auto& p : snapshot.processes) {
    if (p.running || p.exit_code == 0) continue;
    if (!first) out << ",";
    first = false;
    process_json(out, p, true);
  }
  out << "],\"processes\":[";
  first = true;
  for (const auto& p : snapshot.processes) {
    if (!first) out << ",";
    first = false;
    process_json(out, p, true);
  }
  out << "]}";
  return out.str();
}

ManagerStatePeek peek_manager_state(const std::string& msgq_dir) {
  ManagerStatePeek result;
  const std::string path = msgq_dir + "/msgq_managerState";
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) {
    result.json = unavailable_json(path, "no managerState queue: openpilot's manager is not running");
    return result;
  }
  result.queue_exists = true;
  commaview::gps::QueuePeek queue(path);
  const auto message = queue.newest();
  if (!message) {
    result.json = unavailable_json(path, "no whole managerState message in the queue right now");
    return result;
  }
  const auto snapshot = manager_state_from_event(message->data(), message->size());
  if (!snapshot) {
    result.json = unavailable_json(path, "the newest managerState message did not decode with this build's schema");
    return result;
  }
  result.available = true;
  result.json = manager_state_json(*snapshot, monotonic_ns());
  return result;
}

}  // namespace commaview::support
