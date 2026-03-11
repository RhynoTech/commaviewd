#pragma once

#include <string>

#include "cereal/gen/cpp/log.capnp.h"

namespace commaview::telemetry {

std::string build_telemetry_json(cereal::Event::Reader event);

}  // namespace commaview::telemetry
