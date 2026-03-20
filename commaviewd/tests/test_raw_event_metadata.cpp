#include <cassert>
#include <cstdint>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "raw_event_metadata.h"
#include "cereal/gen/cpp/log.capnp.h"

int main() {
  capnp::MallocMessageBuilder mb;
  auto evt = mb.initRoot<cereal::Event>();
  evt.setLogMonoTime(424242ULL);
  evt.initCarState();

  auto words = capnp::messageToFlatArray(mb);
  auto bytes = words.asBytes();

  uint64_t log_mono_time = 0;
  assert(commaview::telemetry::extract_log_mono_time_from_raw_event(
      reinterpret_cast<const uint8_t*>(bytes.begin()), bytes.size(), &log_mono_time));
  assert(log_mono_time == 424242ULL);

  assert(commaview::telemetry::event_which_for_service_index(0) == static_cast<uint16_t>(cereal::Event::CAR_STATE));
  assert(commaview::telemetry::event_which_for_service_index(1) == static_cast<uint16_t>(cereal::Event::SELFDRIVE_STATE));
  assert(commaview::telemetry::event_which_for_service_index(12) == static_cast<uint16_t>(cereal::Event::ROAD_CAMERA_STATE));
  assert(commaview::telemetry::event_which_for_service_index(-1) == 0);
  assert(commaview::telemetry::event_which_for_service_index(99) == 0);

  assert(!commaview::telemetry::extract_log_mono_time_from_raw_event(nullptr, 0, &log_mono_time));
  assert(!commaview::telemetry::extract_log_mono_time_from_raw_event(
      reinterpret_cast<const uint8_t*>(bytes.begin()), 8, &log_mono_time));

  return 0;
}
