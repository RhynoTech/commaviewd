#include "json_builder.h"

#include <capnp/message.h>
#include <cassert>
#include <string>

#include "cereal/gen/cpp/log.capnp.h"

namespace {

bool has(const std::string& s, const std::string& needle) {
  return s.find(needle) != std::string::npos;
}

void set_xyz(cereal::XYZTData::Builder xyz,
             float x0, float x1,
             float y0, float y1,
             float z0, float z1) {
  auto x = xyz.initX(2);
  x.set(0, x0);
  x.set(1, x1);

  auto y = xyz.initY(2);
  y.set(0, y0);
  y.set(1, y1);

  auto z = xyz.initZ(2);
  z.set(0, z0);
  z.set(1, z1);
}

void test_selfdrive_json_shape() {
  capnp::MallocMessageBuilder mb;
  auto evt = mb.initRoot<cereal::Event>();
  auto ss = evt.initSelfdriveState();
  ss.setEnabled(true);
  ss.setAlertText1("Hello");
  ss.setAlertText2("World");
  ss.setAlertType("test");

  std::string out = commaview::telemetry::build_telemetry_json(evt.asReader());
  assert(has(out, "\"type\":\"controlsState\""));
  assert(has(out, "\"enabled\":true"));
  assert(has(out, "\"alertText1\":\"Hello\""));
}

void test_model_v2_parity_contract_fields() {
  capnp::MallocMessageBuilder mb;
  auto evt = mb.initRoot<cereal::Event>();
  evt.setLogMonoTime(4242);

  auto model = evt.initModelV2();
  model.setFrameId(77);
  model.setFrameIdExtra(88);
  model.setFrameAge(3);
  model.setFrameDropPerc(0.25f);
  model.setTimestampEof(123456789ULL);

  auto lane_lines = model.initLaneLines(1);
  set_xyz(lane_lines[0], 0.0f, 1.0f, 0.1f, 0.2f, 0.0f, 0.0f);

  auto lane_probs = model.initLaneLineProbs(1);
  lane_probs.set(0, 0.9f);
  auto lane_stds = model.initLaneLineStds(1);
  lane_stds.set(0, 0.2f);

  auto road_edges = model.initRoadEdges(1);
  set_xyz(road_edges[0], 0.0f, 1.0f, 2.0f, 2.1f, 0.0f, 0.0f);

  auto edge_stds = model.initRoadEdgeStds(1);
  edge_stds.set(0, 0.3f);

  auto pos = model.initPosition();
  set_xyz(pos, 0.0f, 1.0f, 0.0f, 0.1f, 0.0f, 0.0f);

  auto leads = model.initLeadsV3(1);
  leads[0].setProb(0.8f);
  auto lead_x = leads[0].initX(2);
  lead_x.set(0, 20.0f);
  lead_x.set(1, 22.0f);
  auto lead_y = leads[0].initY(2);
  lead_y.set(0, 0.2f);
  lead_y.set(1, 0.3f);

  std::string out = commaview::telemetry::build_telemetry_json(evt.asReader());

  assert(has(out, "\"type\":\"modelV2\""));
  assert(has(out, "\"laneLines\""));
  assert(has(out, "\"roadEdges\""));
  assert(has(out, "\"laneLineStds\""));
  assert(has(out, "\"roadEdgeStds\""));
  assert(has(out, "\"logMonoTime\":4242"));

  // Added for parity-grade frame association in COM-11.
  assert(has(out, "\"frameId\":77"));
  assert(has(out, "\"frameIdExtra\":88"));
  assert(has(out, "\"timestampEof\":123456789"));
}

void test_radar_contract_fields() {
  capnp::MallocMessageBuilder mb;
  auto evt = mb.initRoot<cereal::Event>();
  evt.setLogMonoTime(9001);

  auto radar = evt.initRadarState();
  auto l1 = radar.initLeadOne();
  l1.setDRel(20.0f);
  l1.setYRel(0.2f);
  l1.setVRel(-1.2f);
  l1.setARel(0.4f);
  l1.setStatus(true);

  std::string out = commaview::telemetry::build_telemetry_json(evt.asReader());
  assert(has(out, "\"type\":\"radarState\""));
  assert(has(out, "\"yRel\":0.200000"));
  assert(has(out, "\"aRel\":0.400000"));
  assert(has(out, "\"logMonoTime\":9001"));
}

void test_live_calibration_log_mono_time() {
  capnp::MallocMessageBuilder mb;
  auto evt = mb.initRoot<cereal::Event>();
  evt.setLogMonoTime(31415);

  auto calib = evt.initLiveCalibration();
  calib.setCalStatus(cereal::LiveCalibrationData::Status::CALIBRATED);
  calib.setCalPerc(88);
  auto rpy = calib.initRpyCalib(3);
  rpy.set(0, 0.0f);
  rpy.set(1, 0.01f);
  rpy.set(2, -0.02f);
  auto height = calib.initHeight(1);
  height.set(0, 1.22f);

  std::string out = commaview::telemetry::build_telemetry_json(evt.asReader());
  assert(has(out, "\"type\":\"liveCalibration\""));
  assert(has(out, "\"logMonoTime\":31415"));
  assert(has(out, "\"calPerc\":88"));
}

}  // namespace

int main() {
  test_selfdrive_json_shape();
  test_model_v2_parity_contract_fields();
  test_radar_contract_fields();
  test_live_calibration_log_mono_time();
  return 0;
}
