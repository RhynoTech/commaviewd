// openpilot's process table read from its managerState msgq ring without subscribing, for the
// support bundle: which processes should run but don't, exit codes, and a header left untouched.
#include "manager_state_peek.h"

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cereal/gen/cpp/log.capnp.h"
#include "msgq/msgq.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    failures++;
  }
}

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// A queue file laid out as msgq lays it out, written the way msgq_msg_send writes it.
struct FakeQueue {
  std::string path;
  size_t size;
  uint32_t lap = 0;
  uint64_t offset = 0;

  FakeQueue(std::string p, size_t data_size) : path(std::move(p)), size(data_size) {
    std::vector<char> zeros(sizeof(msgq_header_t) + size, 0);
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(zeros.data(), 1, zeros.size(), f);
    std::fclose(f);
  }

  void write_at(uint64_t pos, const void* bytes, size_t n) {
    FILE* f = std::fopen(path.c_str(), "r+b");
    std::fseek(f, static_cast<long>(pos), SEEK_SET);
    std::fwrite(bytes, 1, n, f);
    std::fclose(f);
  }

  void publish(const std::vector<uint8_t>& message) {
    const uint64_t total = (message.size() + sizeof(int64_t) + 7) & ~uint64_t{7};
    const int64_t remaining = static_cast<int64_t>(size) - static_cast<int64_t>(offset) - static_cast<int64_t>(total) - 8;
    if (remaining <= 0) {
      const int64_t wrap = -1;
      write_at(sizeof(msgq_header_t) + offset, &wrap, sizeof(wrap));
      offset = 0;
      lap++;
    }
    const int64_t n = static_cast<int64_t>(message.size());
    write_at(sizeof(msgq_header_t) + offset, &n, sizeof(n));
    write_at(sizeof(msgq_header_t) + offset + sizeof(int64_t), message.data(), message.size());
    offset = (offset + sizeof(int64_t) + message.size() + 7) & ~uint64_t{7};
    const uint64_t pointer = (uint64_t{lap} << 32) | offset;
    write_at(offsetof(msgq_header_t, write_pointer), &pointer, sizeof(pointer));
  }

  std::vector<char> header() const {
    std::vector<char> out(sizeof(msgq_header_t));
    FILE* f = std::fopen(path.c_str(), "rb");
    size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    assert(got == out.size());
    return out;
  }
};

struct Proc {
  const char* name;
  int32_t pid;
  bool running;
  bool should_be_running;
  int32_t exit_code;
};

std::vector<uint8_t> manager_state_event(const std::vector<Proc>& procs, uint64_t mono_ns) {
  capnp::MallocMessageBuilder builder;
  auto event = builder.initRoot<cereal::Event>();
  event.setLogMonoTime(mono_ns);
  auto list = event.initManagerState().initProcesses(static_cast<unsigned>(procs.size()));
  for (size_t i = 0; i < procs.size(); i++) {
    list[i].setName(procs[i].name);
    list[i].setPid(procs[i].pid);
    list[i].setRunning(procs[i].running);
    list[i].setShouldBeRunning(procs[i].should_be_running);
    list[i].setExitCode(procs[i].exit_code);
  }
  auto words = capnp::messageToFlatArray(builder);
  auto bytes = words.asBytes();
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

std::vector<uint8_t> car_event() {
  capnp::MallocMessageBuilder builder;
  builder.initRoot<cereal::Event>().initCarState().setVEgo(20.0f);
  auto words = capnp::messageToFlatArray(builder);
  auto bytes = words.asBytes();
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

const std::vector<Proc> kCrashedTable = {
    {"controlsd", 100, true, true, 0},
    {"modeld", 0, false, true, -9},
    {"ui", 0, false, false, 1},
    {"camerad", 0, false, true, 0},
};

}  // namespace

int main(int argc, char** argv) {
  // --write-manager-state-queue PATH writes a managerState queue (modeld killed by SIGKILL while it
  // should run) with this build's own msgq and schema, for the support logs integration test.
  if (argc == 3 && std::string(argv[1]) == "--write-manager-state-queue") {
    FakeQueue queue(argv[2], 1 << 16);
    queue.publish(manager_state_event(kCrashedTable, 1000000000ULL));
    return 0;
  }

  using commaview::support::manager_state_from_event;
  using commaview::support::manager_state_json;
  using commaview::support::peek_manager_state;

  const auto event = manager_state_event(kCrashedTable, 5000000000ULL);
  const auto snapshot = manager_state_from_event(event.data(), event.size());
  check(snapshot.has_value() && snapshot->processes.size() == 4, "the process table decodes");
  check(snapshot.has_value() && snapshot->log_mono_ns == 5000000000ULL, "it keeps when manager published it");
  if (snapshot) {
    const auto& modeld = snapshot->processes[1];
    check(modeld.name == "modeld" && !modeld.running && modeld.should_be_running && modeld.exit_code == -9,
          "each process keeps its state");
    const std::string json = manager_state_json(*snapshot, 6500000000ULL);
    check(has(json, "\"available\":true") && has(json, "\"processCount\":4") && has(json, "\"ageMs\":1500"),
          "summary fields: " + json);
    const std::string not_running = json.substr(json.find("\"shouldRunButNotRunning\""),
                                                json.find("\"exitedWithError\"") - json.find("\"shouldRunButNotRunning\""));
    check(has(not_running, "\"name\":\"modeld\"") && has(not_running, "\"exitCode\":-9") &&
              has(not_running, "\"exitSignal\":9") && has(not_running, "\"name\":\"camerad\"") &&
              !has(not_running, "controlsd") && !has(not_running, "\"name\":\"ui\""),
          "should run but not running: " + not_running);
    const std::string exited = json.substr(json.find("\"exitedWithError\""),
                                           json.find("\"processes\"") - json.find("\"exitedWithError\""));
    check(has(exited, "\"name\":\"modeld\"") && has(exited, "\"name\":\"ui\"") && has(exited, "\"exitCode\":1") &&
              !has(exited, "controlsd") && !has(exited, "camerad"),
          "exited with an error: " + exited);
    check(has(json, "{\"name\":\"controlsd\",\"running\":true,\"shouldBeRunning\":true,\"pid\":100,\"exitCode\":0}"),
          "the whole table is kept");
    check(!has(manager_state_json(*snapshot, 0), "ageMs"), "no clock, no age");
  }

  const auto other = car_event();
  check(!manager_state_from_event(other.data(), other.size()).has_value(), "another event is not a process table");
  std::vector<uint8_t> junk(64, 0xAB);
  check(!manager_state_from_event(junk.data(), junk.size()).has_value(), "junk is not a process table");
  check(!manager_state_from_event(junk.data(), 7).has_value(), "a torn message is not a process table");

  char dir_template[] = "/tmp/commaview-manager-state-XXXXXX";
  const std::string dir = mkdtemp(dir_template);

  auto missing = peek_manager_state(dir);
  check(!missing.queue_exists && !missing.available && has(missing.json, "\"available\":false"),
        "no queue: openpilot isn't running");

  FakeQueue queue(dir + "/msgq_managerState", 1 << 16);
  auto empty = peek_manager_state(dir);
  check(empty.queue_exists && !empty.available && has(empty.json, "\"reason\""), "an empty queue says why");

  queue.publish(manager_state_event({{"controlsd", 100, true, true, 0}}, 1000000000ULL));
  queue.publish(manager_state_event(kCrashedTable, 2000000000ULL));
  const auto before = queue.header();
  auto peeked = peek_manager_state(dir);
  check(peeked.available && has(peeked.json, "\"processCount\":4") && has(peeked.json, "\"exitSignal\":9"),
        "the newest process table is read: " + peeked.json);
  check(queue.header() == before, "the queue's header is untouched: no reader slot, no read pointer");

  queue.publish(car_event());
  auto wrong = peek_manager_state(dir);
  check(wrong.queue_exists && !wrong.available && has(wrong.json, "did not decode"), "a foreign message is reported");

  // A ring that wraps many times still yields the newest table.
  const std::string small_dir = dir + "/small";
  check(mkdir(small_dir.c_str(), 0755) == 0, "small ring dir made");
  FakeQueue wrapped(small_dir + "/msgq_managerState", 4096);
  for (int i = 0; i < 60; i++) {
    wrapped.publish(manager_state_event({{"modeld", i, i % 2 == 0, true, -i}}, 1000000000ULL + i));
  }
  check(wrapped.lap > 0, "the small ring wrapped");
  auto newest = peek_manager_state(small_dir);
  check(newest.available && has(newest.json, "\"pid\":59") && has(newest.json, "\"exitCode\":-59"),
        "after wrapping, the newest table: " + newest.json);

  std::string cleanup = "rm -rf '" + dir + "'";
  if (std::system(cleanup.c_str()) != 0) std::fprintf(stderr, "warning: could not remove %s\n", dir.c_str());
  if (failures == 0) std::printf("PASS: managerState read from msgq without subscribing\n");
  return failures == 0 ? 0 : 1;
}
