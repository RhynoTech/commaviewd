#include "commaview/runtime/bridge_mode.h"

int commaview_bridge_main(int argc, char* argv[]);

namespace commaview::runtime {

int run_bridge_mode(int argc, char* argv[]) {
  return commaview_bridge_main(argc, argv);
}

}  // namespace commaview::runtime
