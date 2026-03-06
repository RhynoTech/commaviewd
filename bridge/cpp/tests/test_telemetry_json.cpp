#include "commaview/telemetry/json_builder.h"

#include <capnp/message.h>
#include <cassert>
#include <string>

#include "cereal/gen/cpp/log.capnp.h"

int main() {
  capnp::MallocMessageBuilder mb;
  auto evt = mb.initRoot<cereal::Event>();
  auto ss = evt.initSelfdriveState();
  ss.setEnabled(true);
  ss.setAlertText1("Hello");
  ss.setAlertText2("World");
  ss.setAlertType("test");

  std::string out = commaview::telemetry::build_telemetry_json(evt.asReader());
  assert(out.find("\"type\":\"controlsState\"") != std::string::npos);
  assert(out.find("\"enabled\":true") != std::string::npos);
  assert(out.find("\"alertText1\":\"Hello\"") != std::string::npos);
  return 0;
}
