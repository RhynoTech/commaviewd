#include "commaview/runtime/bridge_mode.h"
#include "commaview/runtime/control_mode.h"
#include "commaview/runtime/mode.h"

#include <cstdio>

int main(int argc, char* argv[]) {
  auto parsed = commaview::runtime::parse_mode(argc, argv);
  if (!parsed.ok) {
    if (parsed.error != "help") {
      std::fprintf(stderr, "commaviewd: %s\n", parsed.error.c_str());
    }
    std::fprintf(stderr, "%s", commaview::runtime::mode_usage(argc > 0 ? argv[0] : "commaviewd").c_str());
    return parsed.error == "help" ? 0 : 2;
  }

  if (parsed.mode == commaview::runtime::RuntimeMode::kBridge) {
    return commaview::runtime::run_bridge_mode(argc, argv);
  }

  return commaview::runtime::run_control_mode(argc, argv);
}
