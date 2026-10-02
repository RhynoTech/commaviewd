// offroad / parked / driving from carState and selfdriveState read straight from their msgq rings,
// without subscribing. Anything that can't show the car is parked is driving.
//
// --write-road-queues DIR GEAR STANDSTILL ENABLED [AGE_MS] writes msgq_carState and
// msgq_selfdriveState queue files laid out by this build's own msgq and schema (GEAR is unknown,
// park or drive), for the control API's integration tests.
#include "road_phase.h"

#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "cereal/gen/cpp/log.capnp.h"
#include "msgq/msgq.h"

using commaview::road::CarSample;
using commaview::road::RoadPhase;
using commaview::road::RoadPhaseInputs;
using commaview::road::SelfdriveSample;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    failures++;
  }
}

uint64_t now_ns() {
  timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

// A queue file laid out as msgq lays it out, written the way msgq_msg_send writes it.
void write_queue(const std::string& path, const std::vector<std::vector<uint8_t>>& messages) {
  const size_t size = 1 << 16;
  std::vector<char> file(sizeof(msgq_header_t) + size, 0);
  uint64_t offset = 0;
  for (const auto& message : messages) {
    const int64_t n = static_cast<int64_t>(message.size());
    std::memcpy(&file[sizeof(msgq_header_t) + offset], &n, sizeof(n));
    std::memcpy(&file[sizeof(msgq_header_t) + offset + sizeof(n)], message.data(), message.size());
    offset = (offset + sizeof(n) + message.size() + 7) & ~uint64_t{7};
  }
  std::memcpy(&file[offsetof(msgq_header_t, write_pointer)], &offset, sizeof(offset));
  FILE* f = std::fopen(path.c_str(), "wb");
  std::fwrite(file.data(), 1, file.size(), f);
  std::fclose(f);
}

std::vector<uint8_t> bytes_of(capnp::MallocMessageBuilder& builder) {
  auto words = capnp::messageToFlatArray(builder);
  auto bytes = words.asBytes();
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

cereal::CarState::GearShifter gear_named(const std::string& name) {
  if (name == "park") return cereal::CarState::GearShifter::PARK;
  if (name == "drive") return cereal::CarState::GearShifter::DRIVE;
  return cereal::CarState::GearShifter::UNKNOWN;
}

std::vector<uint8_t> car_event(cereal::CarState::GearShifter gear, bool standstill, uint64_t mono_ns) {
  capnp::MallocMessageBuilder builder;
  auto event = builder.initRoot<cereal::Event>();
  event.setLogMonoTime(mono_ns);
  auto car = event.initCarState();
  car.setGearShifter(gear);
  car.setStandstill(standstill);
  car.setVEgo(standstill ? 0.0f : 12.0f);
  return bytes_of(builder);
}

std::vector<uint8_t> selfdrive_event(bool enabled, uint64_t mono_ns) {
  capnp::MallocMessageBuilder builder;
  auto event = builder.initRoot<cereal::Event>();
  event.setLogMonoTime(mono_ns);
  event.initSelfdriveState().setEnabled(enabled);
  return bytes_of(builder);
}

RoadPhaseInputs parked_inputs(uint64_t now) {
  RoadPhaseInputs in;
  in.onroad = true;
  in.now_mono_ns = now;
  in.car = CarSample{now - 10'000'000, true, true, true};
  in.selfdrive = SelfdriveSample{now - 10'000'000, false};
  return in;
}

void test_decisions() {
  using commaview::road::decide_road_phase;
  const uint64_t now = 100ULL * 1000 * 1000 * 1000;

  RoadPhaseInputs off;
  off.onroad = false;
  check(decide_road_phase(off).phase == RoadPhase::kOffroad, "IsOffroad says offroad");

  check(decide_road_phase(parked_inputs(now)).phase == RoadPhase::kParked, "park + standstill + not engaged is parked");

  auto in = parked_inputs(now);
  in.car.reset();
  check(decide_road_phase(in).phase == RoadPhase::kDriving, "no carState is driving");

  in = parked_inputs(now);
  in.car->gear_known = false;
  in.car->in_park = false;
  auto reading = decide_road_phase(in);
  check(reading.phase == RoadPhase::kDriving && reading.reason == "gear-unknown", "a car that reports no gear is never guessed parked");

  in = parked_inputs(now);
  in.car->in_park = false;
  check(decide_road_phase(in).phase == RoadPhase::kDriving, "drive is driving, even stopped");

  in = parked_inputs(now);
  in.car->standstill = false;
  check(decide_road_phase(in).phase == RoadPhase::kDriving, "rolling in park is driving");

  in = parked_inputs(now);
  in.selfdrive->enabled = true;
  check(decide_road_phase(in).reason == "engaged", "engaged is driving");

  in = parked_inputs(now);
  in.car->log_mono_ns = now - 1'500'000'000;
  check(decide_road_phase(in).reason == "car-state-stale", "a carState over 1 s old is driving");

  in = parked_inputs(now);
  in.selfdrive->log_mono_ns = now - 1'500'000'000;
  check(decide_road_phase(in).reason == "selfdrive-state-stale", "a selfdriveState over 1 s old is driving");

  in = parked_inputs(now);
  in.selfdrive.reset();
  check(decide_road_phase(in).phase == RoadPhase::kDriving, "no selfdriveState is driving");

  // sunnypilot: MADS lateral can be on while openpilot is not engaged.
  in = parked_inputs(now);
  in.mads_queue_exists = true;
  check(decide_road_phase(in).reason == "mads-state-unknown", "an unreadable MADS state is driving");
  in.mads = SelfdriveSample{now - 10'000'000, true};
  check(decide_road_phase(in).reason == "mads-engaged", "MADS engaged is driving");
  in.mads->enabled = false;
  check(decide_road_phase(in).phase == RoadPhase::kParked, "MADS off and parked is parked");
}

void test_events() {
  const uint64_t now = now_ns();
  const auto car = car_event(cereal::CarState::GearShifter::PARK, true, now);
  const auto sample = commaview::road::car_sample_from_event(car.data(), car.size());
  check(sample && sample->gear_known && sample->in_park && sample->standstill && sample->log_mono_ns == now,
        "carState gear, standstill and time are read");
  const auto sd = selfdrive_event(true, now);
  const auto sd_sample = commaview::road::selfdrive_sample_from_event(sd.data(), sd.size());
  check(sd_sample && sd_sample->enabled, "selfdriveState.enabled is read");
  check(!commaview::road::car_sample_from_event(sd.data(), sd.size()), "a selfdriveState is no carState");
  check(!commaview::road::mads_sample_from_event(car.data(), car.size()), "a carState is no MADS state");
  // sunnypilot's selfdriveStateSP, built by name: a schema without it skips this.
  try {
    capnp::MallocMessageBuilder builder;
    auto event = builder.initRoot<cereal::Event>();
    event.setLogMonoTime(now);
    capnp::DynamicStruct::Builder dynamic = capnp::toDynamic(event);
    auto sp = dynamic.init("selfdriveStateSP").as<capnp::DynamicStruct>();
    sp.init("mads").as<capnp::DynamicStruct>().set("enabled", true);
    const auto bytes = bytes_of(builder);
    const auto mads = commaview::road::mads_sample_from_event(bytes.data(), bytes.size());
    check(mads && mads->enabled && mads->log_mono_ns == now, "sunnypilot MADS enabled is read");
    std::printf("  schema has selfdriveStateSP: MADS checked\n");
  } catch (const kj::Exception&) {
    std::printf("  schema has no selfdriveStateSP: MADS read skipped\n");
  }
  std::vector<uint8_t> junk(64, 0xAB);
  check(!commaview::road::car_sample_from_event(junk.data(), junk.size()), "junk is no carState");
}

void test_from_queues() {
  char dir_template[] = "/tmp/commaview-road-phase-XXXXXX";
  const std::string dir = mkdtemp(dir_template);
  using commaview::road::read_road_phase;
  check(read_road_phase(false, dir).phase == RoadPhase::kOffroad, "offroad reads no queue");
  check(read_road_phase(true, dir).reason == "car-state-missing", "onroad with no queues is driving");

  const uint64_t now = now_ns();
  write_queue(dir + "/msgq_carState", {car_event(cereal::CarState::GearShifter::DRIVE, false, now - 20'000'000),
                                       car_event(cereal::CarState::GearShifter::PARK, true, now)});
  write_queue(dir + "/msgq_selfdriveState", {selfdrive_event(false, now)});
  struct stat st_before {};
  stat((dir + "/msgq_carState").c_str(), &st_before);
  const auto reading = read_road_phase(true, dir);
  check(reading.phase == RoadPhase::kParked, "the newest carState (park, standstill) and a disengaged selfdriveState are parked");

  write_queue(dir + "/msgq_selfdriveState", {selfdrive_event(true, now)});
  check(read_road_phase(true, dir).reason == "engaged", "engaged from the queue is driving");

  std::string cleanup = "rm -rf '" + dir + "'";
  if (std::system(cleanup.c_str()) != 0) std::fprintf(stderr, "warning: could not remove %s\n", dir.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 6 && std::string(argv[1]) == "--write-road-queues") {
    const std::string dir = argv[2];
    const uint64_t age_ns = argc >= 7 ? std::strtoull(argv[6], nullptr, 10) * 1000000ULL : 0;
    const uint64_t at = now_ns() - age_ns;
    write_queue(dir + "/msgq_carState", {car_event(gear_named(argv[3]), std::string(argv[4]) == "1", at)});
    write_queue(dir + "/msgq_selfdriveState", {selfdrive_event(std::string(argv[5]) == "1", at)});
    return 0;
  }
  test_decisions();
  test_events();
  test_from_queues();
  if (failures == 0) std::printf("PASS: road phase (offroad / parked / driving) read without subscribing\n");
  return failures == 0 ? 0 : 1;
}
