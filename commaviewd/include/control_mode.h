#pragma once

namespace commaview::runtime {

int run_control_mode(int argc, char* argv[]);

// Prints "<phase> <reason>" (offroad, parked or driving) and returns 0 when maintenance may run
// (offroad, or parked: in Park, at a standstill, not engaged), 1 while driving.
int run_road_phase_mode();

}  // namespace commaview::runtime
