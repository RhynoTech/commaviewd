#pragma once

#include <string>

namespace commaview::runtime {

enum class RuntimeMode {
  kBridge,
  kControl,
  // One-shot: prints the road phase and exits (the maintenance gate scripts read).
  kRoadPhase,
};

struct ParsedMode {
  bool ok = false;
  RuntimeMode mode = RuntimeMode::kBridge;
  int mode_arg_index = -1;
  std::string error;
};

ParsedMode parse_mode(int argc, char* argv[]);

std::string mode_usage(const char* argv0);

}  // namespace commaview::runtime
