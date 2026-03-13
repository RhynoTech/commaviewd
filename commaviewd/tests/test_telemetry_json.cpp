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
  evt.setLogMonoTime(111);
  auto ss = evt.initSelfdriveState();
  ss.setEnabled(true);
  ss.setActive(true);
  ss.setEngageable(true);
  ss.setAlertText1("Hello");
  ss.setAlertText2("World");
  ss.setAlertType("test");

  std::string out = commaview::telemetry::build_telemetry_json(evt.asReader());
  assert(has(out, "\"type\":\"controlsState\""));
  assert(has(out, "\"enabled\":true"));
  assert(has(out, "\"alertText1\":\"Hello\""));
  assert(has(out, "\"active\":true"));
  assert(has(out, "\"engageable\":true"));
  assert(has(out, "\"logMonoTime\":111"));
}

void test_car_state_rich_contract_fields() {
  capnp::MallocMessageBuilder mb;
  auto evt = mb.initRoot<cereal::Event>();
  evt.setLogMonoTime(222);

  auto cs = evt.initCarState();
  cs.setVEgo(12.3f);
  cs.setSteeringAngleDeg(-1.5f);
  cs.setBrakePressed(true);
  cs.setGasPressed(false);
  cs.setCanValid(true);
  cs.setCanTimeout(false);
  cs.setCanErrorCounter(7);
  cs.setStandstill(false);
  cs.setLeftBlinker(true);
  cs.setRightBlinker(false);

  auto wheel = cs.initWheelSpeeds();
  wheel.setFl(12.0f);
  wheel.setFr(12.1f);
  wheel.setRl(11.9f);
  wheel.setRr(12.2f);

  auto cruise = cs.initCruiseState();
  cruise.setEnabled(true);
  cruise.setSpeed(22.2f);
  cruise.setAvailable(true);

  std::string out = commaview::telemetry::build_telemetry_json(evt.asReader());
  assert(has(out, "\"type\":\"carState\""));
  assert(has(out, "\"speed\":12.3"));
  assert(has(out, "\"steeringAngle\":-1.5"));
  assert(has(out, "\"canValid\":true"));
  assert(has(out, "\"canErrorCounter\":7"));
  assert(has(out, "\"wheelSpeeds\""));
  assert(has(out, "\"cruiseState\""));
  assert(has(out, "\"logMonoTime\":222"));
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
  assert(has(out, "\"yRel\":0.2"));
  assert(has(out, "\"aRel\":0.4"));
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

void test_car_control_and_output_json() {
  {
    capnp::MallocMessageBuilder mb;
    auto evt = mb.initRoot<cereal::Event>();
    evt.setLogMonoTime(777);
    auto cc = evt.initCarControl();
    cc.setEnabled(true);
    cc.setLatActive(true);
    cc.setLongActive(false);
    cc.setCurrentCurvature(0.01f);
    auto act = cc.initActuators();
    act.setAccel(1.5f);
    act.setLongControlState(cereal::CarControl::Actuators::LongControlState::PID);

    std::string out = commaview::telemetry::build_telemetry_json(evt.asReader());
    assert(has(out, "\"type\":\"carControl\""));
    assert(has(out, "\"enabled\":true"));
    assert(has(out, "\"latActive\":true"));
    assert(has(out, "\"actuators\""));
    assert(has(out, "\"logMonoTime\":777"));
  }

  {
    capnp::MallocMessageBuilder mb;
    auto evt = mb.initRoot<cereal::Event>();
    evt.setLogMonoTime(778);
    auto co = evt.initCarOutput();
    auto out_act = co.initActuatorsOutput();
    out_act.setAccel(1.1f);
    out_act.setSpeed(15.0f);

    std::string out = commaview::telemetry::build_telemetry_json(evt.asReader());
    assert(has(out, "\"type\":\"carOutput\""));
    assert(has(out, "\"actuatorsOutput\""));
    assert(has(out, "\"logMonoTime\":778"));
  }
}

void test_live_parameters_json() {
  capnp::MallocMessageBuilder mb;
  auto evt = mb.initRoot<cereal::Event>();
  evt.setLogMonoTime(888);
  auto lp = evt.initLiveParameters();
  lp.setValid(true);
  lp.setSensorValid(true);
  lp.setPosenetValid(true);
  lp.setSteerRatio(16.7f);
  lp.setSteerRatioStd(0.2f);

  std::string out = commaview::telemetry::build_telemetry_json(evt.asReader());
  assert(has(out, "\"type\":\"liveParameters\""));
  assert(has(out, "\"valid\":true"));
  assert(has(out, "\"steerRatio\":16.7"));
  assert(has(out, "\"steerRatioStd\":0.2"));
  assert(has(out, "\"logMonoTime\":888"));
}

void test_driver_monitoring_and_driver_state_v2_json() {
  {
    capnp::MallocMessageBuilder mb;
    auto evt = mb.initRoot<cereal::Event>();
    evt.setLogMonoTime(999);
    auto dm = evt.initDriverMonitoringState();
    dm.setFaceDetected(true);
    dm.setIsDistracted(true);
    dm.setAwarenessStatus(0.5f);
    auto events = dm.initEvents(1);
    events[0].setWarning(true);
    events[0].setPermanent(true);

    std::string out = commaview::telemetry::build_telemetry_json(evt.asReader());
    assert(has(out, "\"type\":\"driverMonitoringState\""));
    assert(has(out, "\"faceDetected\":true"));
    assert(has(out, "\"isDistracted\":true"));
    assert(has(out, "\"events\""));
    assert(has(out, "\"logMonoTime\":999"));
  }

  {
    capnp::MallocMessageBuilder mb;
    auto evt = mb.initRoot<cereal::Event>();
    evt.setLogMonoTime(1000);
    auto ds = evt.initDriverStateV2();
    ds.setFrameId(5);
    ds.setModelExecutionTime(0.03f);
    ds.setWheelOnRightProb(0.7f);
    auto left = ds.initLeftDriverData();
    left.setFaceProb(0.9f);
    left.setPhoneProb(0.1f);
    auto right = ds.initRightDriverData();
    right.setFaceProb(0.2f);

    std::string out = commaview::telemetry::build_telemetry_json(evt.asReader());
    assert(has(out, "\"type\":\"driverStateV2\""));
    assert(has(out, "\"frameId\":5"));
    assert(has(out, "\"leftDriverData\""));
    assert(has(out, "\"rightDriverData\""));
    assert(has(out, "\"logMonoTime\":1000"));
  }
}

void test_onroad_events_json() {
  capnp::MallocMessageBuilder mb;
  auto evt = mb.initRoot<cereal::Event>();
  evt.setLogMonoTime(1111);
  auto events = evt.initOnroadEvents(2);
  events[0].setWarning(true);
  events[0].setPermanent(true);
  events[1].setEnable(true);

  std::string out = commaview::telemetry::build_telemetry_json(evt.asReader());
  assert(has(out, "\"type\":\"onroadEvents\""));
  assert(has(out, "\"count\":2"));
  assert(has(out, "\"warning\":true"));
  assert(has(out, "\"logMonoTime\":1111"));
}

void test_road_camera_state_json() {
  capnp::MallocMessageBuilder mb;
  auto evt = mb.initRoot<cereal::Event>();
  evt.setLogMonoTime(1212);
  auto frame = evt.initRoadCameraState();
  frame.setFrameId(44);
  frame.setFrameIdSensor(45);
  frame.setRequestId(46);
  frame.setEncodeId(47);
  frame.setTimestampSof(1000);
  frame.setTimestampEof(1015);
  frame.setProcessingTime(0.8f);
  frame.setIntegLines(120);
  frame.setGain(1.7f);
  frame.setHighConversionGain(true);
  frame.setMeasuredGreyFraction(0.4f);
  frame.setTargetGreyFraction(0.6f);
  frame.setExposureValPercent(55.0f);
  auto temps = frame.initTemperaturesC(2);
  temps.set(0, 52.5f);
  temps.set(1, 53.0f);

  std::string out = commaview::telemetry::build_telemetry_json(evt.asReader());
  assert(has(out, "\"type\":\"roadCameraState\""));
  assert(has(out, "\"frameId\":44"));
  assert(has(out, "\"frameIdSensor\":45"));
  assert(has(out, "\"highConversionGain\":true"));
  assert(has(out, "\"temperaturesC\""));
  assert(has(out, "\"logMonoTime\":1212"));
}

}  // namespace

int main() {
  test_selfdrive_json_shape();
  test_car_state_rich_contract_fields();
  test_model_v2_parity_contract_fields();
  test_radar_contract_fields();
  test_live_calibration_log_mono_time();
  test_car_control_and_output_json();
  test_live_parameters_json();
  test_driver_monitoring_and_driver_state_v2_json();
  test_onroad_events_json();
  test_road_camera_state_json();
  return 0;
}
