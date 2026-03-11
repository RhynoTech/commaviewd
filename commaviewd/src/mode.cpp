#include "mode.h"

#include <cstring>

namespace commaview::runtime {
namespace {

bool is_help_flag(const char* arg) {
  return arg != nullptr && (
      std::strcmp(arg, "-h") == 0 ||
      std::strcmp(arg, "--help") == 0 ||
      std::strcmp(arg, "help") == 0);
}

}  // namespace

std::string mode_usage(const char* argv0) {
  const char* prog = (argv0 && argv0[0] != '\0') ? argv0 : "commaviewd";
  std::string usage;
  usage += "Usage:\n";
  usage += "  "; usage += prog; usage += " bridge [bridge flags]\n";
  usage += "  "; usage += prog; usage += " control [control flags]\n";
  usage += "\nModes:\n";
  usage += "  bridge   Run streaming bridge runtime\n";
  usage += "  control  Run control/API runtime\n";
  return usage;
}

ParsedMode parse_mode(int argc, char* argv[]) {
  ParsedMode parsed;

  if (argc <= 0 || argv == nullptr) {
    parsed.error = "invalid argv";
    return parsed;
  }

  if (argc < 2) {
    parsed.error = "missing mode argument";
    return parsed;
  }

  const char* mode = argv[1];
  if (is_help_flag(mode)) {
    parsed.error = "help";
    return parsed;
  }

  if (std::strcmp(mode, "bridge") == 0) {
    parsed.ok = true;
    parsed.mode = RuntimeMode::kBridge;
    parsed.mode_arg_index = 1;
    return parsed;
  }

  if (std::strcmp(mode, "control") == 0) {
    parsed.ok = true;
    parsed.mode = RuntimeMode::kControl;
    parsed.mode_arg_index = 1;
    return parsed;
  }

  parsed.error = "unknown mode: ";
  parsed.error += mode;
  return parsed;
}

}  // namespace commaview::runtime
