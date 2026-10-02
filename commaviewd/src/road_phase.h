#pragma once

// Whether the car is offroad, parked or driving, for the control API.
//
// openpilot itself only knows offroad and onroad (IsOffroad). A car that is on but in Park, at a
// standstill and not engaged is onroad to openpilot but is not being driven; the API reports it as
// "parked". Everything that can't be shown to be parked is "driving": a car that never reports a
// gear, a stale message, an engaged openpilot (or sunnypilot MADS), a missing queue.
//
// carState and selfdriveState (and sunnypilot's selfdriveStateSP when it exists) are read from
// their msgq queues the way gps_peek.h reads GPS: mapped read-only, no subscriber, no reader slot,
// and only when a request asks.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace commaview::road {

enum class RoadPhase { kOffroad, kParked, kDriving };

const char* road_phase_name(RoadPhase phase);

// A message older than this (by its logMonoTime) says nothing about now.
inline constexpr uint64_t kRoadPhaseFreshNs = 1000ULL * 1000 * 1000;

struct CarSample {
  uint64_t log_mono_ns = 0;
  bool gear_known = false;  // gearShifter is not "unknown"
  bool in_park = false;
  bool standstill = false;
};

struct SelfdriveSample {
  uint64_t log_mono_ns = 0;
  bool enabled = false;
};

std::optional<CarSample> car_sample_from_event(const uint8_t* data, size_t size);
std::optional<SelfdriveSample> selfdrive_sample_from_event(const uint8_t* data, size_t size);

struct RoadPhaseInputs {
  bool onroad = true;
  std::optional<CarSample> car;
  std::optional<SelfdriveSample> selfdrive;
  uint64_t now_mono_ns = 0;
};

struct RoadPhaseReading {
  RoadPhase phase = RoadPhase::kDriving;
  // Why, in a word or two: "offroad", "parked", "gear-unknown", "not-in-park", "moving",
  // "engaged", "car-state-missing", "car-state-stale", ...
  std::string reason;
};

RoadPhaseReading decide_road_phase(const RoadPhaseInputs& inputs);

// Reads the queues under msgq_dir (only when onroad) and decides.
RoadPhaseReading read_road_phase(bool onroad, const std::string& msgq_dir);

}  // namespace commaview::road
