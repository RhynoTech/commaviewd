#pragma once

#include <cstddef>
#include <cstdint>

namespace commaview::telemetry {

bool extract_log_mono_time_from_raw_event(const uint8_t* raw, size_t raw_size, uint64_t* log_mono_time);
uint16_t event_which_for_service_index(int service_index);

}  // namespace commaview::telemetry
