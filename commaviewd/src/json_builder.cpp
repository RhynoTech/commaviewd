#include "json_builder.h"

#include <cstdio>
#include <ctime>
#include <type_traits>

namespace commaview::telemetry {
namespace {

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default: out += c;
    }
  }
  return out;
}

std::string json_bool(bool value) {
  return value ? "true" : "false";
}

template <typename T>
std::string json_num(T value) {
  char buf[64];
  if constexpr (std::is_floating_point_v<T>) {
    snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(value));
  } else if constexpr (std::is_signed_v<T>) {
    snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
  } else {
    snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
  }
  return std::string(buf);
}

std::string unix_ts_json() {
  char buf[32];
  snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(time(nullptr)));
  return std::string(buf);
}

template<typename T>
std::string json_float_array(T list, int max_count = -1) {
  std::string s = "[";
  int i = 0;
  for (auto v : list) {
    if (max_count >= 0 && i >= max_count) break;
    if (i > 0) s += ",";
    s += json_num(v);
    i++;
  }
  s += "]";
  return s;
}

std::string build_car_state_json(cereal::CarState::Reader cs, uint64_t log_mono_time) {
  const auto wheel = cs.getWheelSpeeds();
  const auto cruise = cs.getCruiseState();

  std::string s = "{\"ts\":" + unix_ts_json() +
                  ",\"type\":\"carState\",\"data\":{";
  // Legacy keys kept first for compatibility.
  s += "\"speed\":" + json_num(cs.getVEgo()) + ",";
  s += "\"steeringAngle\":" + json_num(cs.getSteeringAngleDeg()) + ",";
  s += "\"brakePressed\":" + json_bool(cs.getBrakePressed()) + ",";
  s += "\"gasPressed\":" + json_bool(cs.getGasPressed()) + ",";

  // Rich parity fields.
  s += "\"aEgo\":" + json_num(cs.getAEgo()) + ",";
  s += "\"vEgoRaw\":" + json_num(cs.getVEgoRaw()) + ",";
  s += "\"vEgoCluster\":" + json_num(cs.getVEgoCluster()) + ",";
  s += "\"vCruise\":" + json_num(cs.getVCruise()) + ",";
  s += "\"vCruiseCluster\":" + json_num(cs.getVCruiseCluster()) + ",";
  s += "\"yawRate\":" + json_num(cs.getYawRate()) + ",";
  s += "\"standstill\":" + json_bool(cs.getStandstill()) + ",";
  s += "\"brake\":" + json_num(cs.getBrake()) + ",";
  s += "\"regenBraking\":" + json_bool(cs.getRegenBraking()) + ",";
  s += "\"parkingBrake\":" + json_bool(cs.getParkingBrake()) + ",";
  s += "\"brakeHoldActive\":" + json_bool(cs.getBrakeHoldActive()) + ",";

  s += "\"steeringRateDeg\":" + json_num(cs.getSteeringRateDeg()) + ",";
  s += "\"steeringTorque\":" + json_num(cs.getSteeringTorque()) + ",";
  s += "\"steeringTorqueEps\":" + json_num(cs.getSteeringTorqueEps()) + ",";
  s += "\"steeringPressed\":" + json_bool(cs.getSteeringPressed()) + ",";
  s += "\"steeringDisengage\":" + json_bool(cs.getSteeringDisengage()) + ",";
  s += "\"steerFaultTemporary\":" + json_bool(cs.getSteerFaultTemporary()) + ",";
  s += "\"steerFaultPermanent\":" + json_bool(cs.getSteerFaultPermanent()) + ",";

  s += "\"canValid\":" + json_bool(cs.getCanValid()) + ",";
  s += "\"canTimeout\":" + json_bool(cs.getCanTimeout()) + ",";
  s += "\"canErrorCounter\":" + json_num(cs.getCanErrorCounter()) + ",";
  s += "\"cumLagMs\":" + json_num(cs.getCumLagMs()) + ",";

  s += "\"leftBlinker\":" + json_bool(cs.getLeftBlinker()) + ",";
  s += "\"rightBlinker\":" + json_bool(cs.getRightBlinker()) + ",";
  s += "\"doorOpen\":" + json_bool(cs.getDoorOpen()) + ",";
  s += "\"seatbeltUnlatched\":" + json_bool(cs.getSeatbeltUnlatched()) + ",";
  s += "\"leftBlindspot\":" + json_bool(cs.getLeftBlindspot()) + ",";
  s += "\"rightBlindspot\":" + json_bool(cs.getRightBlindspot()) + ",";
  s += "\"buttonEnable\":" + json_bool(cs.getButtonEnable()) + ",";

  s += "\"invalidLkasSetting\":" + json_bool(cs.getInvalidLkasSetting()) + ",";
  s += "\"stockAeb\":" + json_bool(cs.getStockAeb()) + ",";
  s += "\"stockLkas\":" + json_bool(cs.getStockLkas()) + ",";
  s += "\"stockFcw\":" + json_bool(cs.getStockFcw()) + ",";
  s += "\"espDisabled\":" + json_bool(cs.getEspDisabled()) + ",";
  s += "\"espActive\":" + json_bool(cs.getEspActive()) + ",";
  s += "\"accFaulted\":" + json_bool(cs.getAccFaulted()) + ",";
  s += "\"vehicleSensorsInvalid\":" + json_bool(cs.getVehicleSensorsInvalid()) + ",";
  s += "\"lowSpeedAlert\":" + json_bool(cs.getLowSpeedAlert()) + ",";
  s += "\"blockPcmEnable\":" + json_bool(cs.getBlockPcmEnable()) + ",";

  s += "\"fuelGauge\":" + json_num(cs.getFuelGauge()) + ",";
  s += "\"charging\":" + json_bool(cs.getCharging()) + ",";
  s += "\"gearShifter\":" + json_num(static_cast<int>(cs.getGearShifter())) + ",";

  s += "\"wheelSpeeds\":{";
  s += "\"fl\":" + json_num(wheel.getFl()) + ",";
  s += "\"fr\":" + json_num(wheel.getFr()) + ",";
  s += "\"rl\":" + json_num(wheel.getRl()) + ",";
  s += "\"rr\":" + json_num(wheel.getRr()) + "},";

  s += "\"cruiseState\":{";
  s += "\"enabled\":" + json_bool(cruise.getEnabled()) + ",";
  s += "\"speed\":" + json_num(cruise.getSpeed()) + ",";
  s += "\"speedCluster\":" + json_num(cruise.getSpeedCluster()) + ",";
  s += "\"available\":" + json_bool(cruise.getAvailable()) + ",";
  s += "\"standstill\":" + json_bool(cruise.getStandstill()) + ",";
  s += "\"nonAdaptive\":" + json_bool(cruise.getNonAdaptive()) + "},";

  s += "\"logMonoTime\":" + json_num(log_mono_time);
  s += "}}";
  return s;
}

std::string build_selfdrive_state_json(cereal::SelfdriveState::Reader ss, uint64_t log_mono_time) {
  std::string s = "{\"ts\":" + unix_ts_json() +
                  ",\"type\":\"controlsState\",\"data\":{";
  // Legacy keys for compatibility.
  s += "\"enabled\":" + json_bool(ss.getEnabled()) + ",";
  s += "\"alertText1\":\"" + json_escape(ss.getAlertText1().cStr()) + "\",";
  s += "\"alertText2\":\"" + json_escape(ss.getAlertText2().cStr()) + "\",";
  s += "\"alertType\":\"" + json_escape(ss.getAlertType().cStr()) + "\",";

  // Rich parity fields.
  s += "\"active\":" + json_bool(ss.getActive()) + ",";
  s += "\"engageable\":" + json_bool(ss.getEngageable()) + ",";
  s += "\"state\":" + json_num(static_cast<int>(ss.getState())) + ",";
  s += "\"alertStatus\":" + json_num(static_cast<int>(ss.getAlertStatus())) + ",";
  s += "\"alertSize\":" + json_num(static_cast<int>(ss.getAlertSize())) + ",";
  s += "\"alertSound\":" + json_num(static_cast<int>(ss.getAlertSound())) + ",";
  s += "\"alertHudVisual\":" + json_num(static_cast<int>(ss.getAlertHudVisual())) + ",";
  s += "\"experimentalMode\":" + json_bool(ss.getExperimentalMode()) + ",";
  s += "\"personality\":" + json_num(static_cast<int>(ss.getPersonality())) + ",";
  s += "\"logMonoTime\":" + json_num(log_mono_time);
  s += "}}";
  return s;
}

std::string build_device_state_json(cereal::DeviceState::Reader ds) {
  float max_cpu = 0.0f;
  for (auto t : ds.getCpuTempC()) if (t > max_cpu) max_cpu = t;
  float max_gpu = 0.0f;
  for (auto t : ds.getGpuTempC()) if (t > max_gpu) max_gpu = t;

  char buf[512];
  snprintf(buf, sizeof(buf),
      "{\"ts\":%.3f,\"type\":\"deviceState\",\"data\":"
      "{\"cpuTempC\":%.1f,\"gpuTempC\":%.1f,"
      "\"memoryUsagePercent\":%d,\"freeSpacePercent\":%.1f,"
      "\"networkStrength\":\"%d\",\"thermalStatus\":\"%d\","
      "\"started\":%s}}",
      static_cast<double>(time(nullptr)),
      static_cast<double>(max_cpu), static_cast<double>(max_gpu),
      ds.getMemoryUsagePercent(), static_cast<double>(ds.getFreeSpacePercent()),
      static_cast<int>(ds.getNetworkStrength()),
      static_cast<int>(ds.getThermalStatus()),
      ds.getStarted() ? "true" : "false");
  return std::string(buf);
}

std::string build_model_v2_json(cereal::ModelDataV2::Reader m, uint64_t log_mono_time) {
  std::string s = "{\"ts\":" + unix_ts_json() +
                  ",\"type\":\"modelV2\",\"data\":{";

  s += "\"frameId\":" + json_num(static_cast<unsigned long long>(m.getFrameId())) + ",";
  s += "\"frameIdExtra\":" + json_num(static_cast<unsigned long long>(m.getFrameIdExtra())) + ",";
  s += "\"frameAge\":" + json_num(static_cast<unsigned long long>(m.getFrameAge())) + ",";
  s += "\"frameDropPerc\":" + json_num(m.getFrameDropPerc()) + ",";
  s += "\"timestampEof\":" + json_num(static_cast<unsigned long long>(m.getTimestampEof())) + ",";

  s += "\"laneLines\":[";
  auto lanes = m.getLaneLines();
  auto lane_probs = m.getLaneLineProbs();
  auto lane_stds = m.getLaneLineStds();
  for (unsigned i = 0; i < lanes.size(); i++) {
    if (i > 0) s += ",";
    const float prob = (i < lane_probs.size()) ? lane_probs[i] : 0.0f;
    const float std = (i < lane_stds.size()) ? lane_stds[i] : 0.0f;
    s += "{\"x\":" + json_float_array(lanes[i].getX()) +
         ",\"y\":" + json_float_array(lanes[i].getY()) +
         ",\"z\":" + json_float_array(lanes[i].getZ()) +
         ",\"prob\":" + json_num(prob) +
         ",\"std\":" + json_num(std) + "}";
  }
  s += "],";

  s += "\"roadEdges\":[";
  auto edges = m.getRoadEdges();
  auto edge_stds = m.getRoadEdgeStds();
  for (unsigned i = 0; i < edges.size(); i++) {
    if (i > 0) s += ",";
    const float std = (i < edge_stds.size()) ? edge_stds[i] : 0.0f;
    s += "{\"x\":" + json_float_array(edges[i].getX()) +
         ",\"y\":" + json_float_array(edges[i].getY()) +
         ",\"z\":" + json_float_array(edges[i].getZ()) +
         ",\"std\":" + json_num(std) + "}";
  }
  s += "],";

  auto pos = m.getPosition();
  s += "\"position\":{\"x\":" + json_float_array(pos.getX()) +
       ",\"y\":" + json_float_array(pos.getY()) +
       ",\"z\":" + json_float_array(pos.getZ()) + "},";

  s += "\"leads\":[";
  auto leads = m.getLeadsV3();
  for (unsigned i = 0; i < leads.size(); i++) {
    if (i > 0) s += ",";
    s += "{\"x\":" + json_float_array(leads[i].getX(), 5) +
         ",\"y\":" + json_float_array(leads[i].getY(), 5) +
         ",\"z\":[]"
         ",\"prob\":" + json_num(leads[i].getProb()) + "}";
  }
  s += "],";

  s += "\"laneLineStds\":" + json_float_array(lane_stds) + ",";
  s += "\"roadEdgeStds\":" + json_float_array(edge_stds) + ",";
  s += "\"logMonoTime\":" + json_num(log_mono_time);
  s += "}}";

  return s;
}

std::string build_radar_state_json(cereal::RadarState::Reader r, uint64_t log_mono_time) {
  auto lead_json = [](cereal::RadarState::LeadData::Reader l) {
    std::string j = "{\"dRel\":" + json_num(l.getDRel()) +
                    ",\"yRel\":" + json_num(l.getYRel()) +
                    ",\"vRel\":" + json_num(l.getVRel()) +
                    ",\"aRel\":" + json_num(l.getARel()) +
                    ",\"status\":" + json_bool(l.getStatus()) + "}";
    return j;
  };

  auto l1 = r.getLeadOne();
  auto l2 = r.getLeadTwo();

  std::string s = "{\"ts\":" + unix_ts_json() +
                  ",\"type\":\"radarState\",\"data\":{";
  s += "\"leadOne\":" + lead_json(l1) + ",";
  s += "\"leadTwo\":" + lead_json(l2) + ",";
  s += "\"logMonoTime\":" + json_num(log_mono_time);
  s += "}}";
  return s;
}

std::string build_live_calibration_json(cereal::LiveCalibrationData::Reader c, uint64_t log_mono_time) {
  std::string s = "{\"ts\":" + unix_ts_json() +
                  ",\"type\":\"liveCalibration\",\"data\":{";
  s += "\"rpyCalib\":" + json_float_array(c.getRpyCalib()) + ",";
  s += "\"height\":" + json_float_array(c.getHeight()) + ",";
  s += "\"calStatus\":\"" + json_num(static_cast<int>(c.getCalStatus())) + "\",";
  s += "\"calStatusInt\":" + json_num(static_cast<int>(c.getCalStatus())) + ",";
  s += "\"calPerc\":" + json_num(c.getCalPerc()) + ",";
  s += "\"logMonoTime\":" + json_num(log_mono_time) + "}}";
  return s;
}

std::string build_car_control_actuators_json(cereal::CarControl::Actuators::Reader a) {
  std::string s = "{";
  s += "\"torque\":" + json_num(a.getTorque()) + ",";
  s += "\"steeringAngleDeg\":" + json_num(a.getSteeringAngleDeg()) + ",";
  s += "\"curvature\":" + json_num(a.getCurvature()) + ",";
  s += "\"accel\":" + json_num(a.getAccel()) + ",";
  s += "\"longControlState\":" + json_num(static_cast<int>(a.getLongControlState())) + ",";
  s += "\"gas\":" + json_num(a.getGas()) + ",";
  s += "\"brake\":" + json_num(a.getBrake()) + ",";
  s += "\"torqueOutputCan\":" + json_num(a.getTorqueOutputCan()) + ",";
  s += "\"speed\":" + json_num(a.getSpeed());
  s += "}";
  return s;
}

std::string build_car_control_json(cereal::CarControl::Reader cc, uint64_t log_mono_time) {
  const auto cruise = cc.getCruiseControl();
  const auto hud = cc.getHudControl();

  std::string s = "{\"ts\":" + unix_ts_json() +
                  ",\"type\":\"carControl\",\"data\":{";
  s += "\"enabled\":" + json_bool(cc.getEnabled()) + ",";
  s += "\"latActive\":" + json_bool(cc.getLatActive()) + ",";
  s += "\"longActive\":" + json_bool(cc.getLongActive()) + ",";
  s += "\"leftBlinker\":" + json_bool(cc.getLeftBlinker()) + ",";
  s += "\"rightBlinker\":" + json_bool(cc.getRightBlinker()) + ",";
  s += "\"currentCurvature\":" + json_num(cc.getCurrentCurvature()) + ",";
  s += "\"orientationNED\":" + json_float_array(cc.getOrientationNED()) + ",";
  s += "\"angularVelocity\":" + json_float_array(cc.getAngularVelocity()) + ",";
  s += "\"actuators\":" + build_car_control_actuators_json(cc.getActuators()) + ",";

  s += "\"cruiseControl\":{";
  s += "\"cancel\":" + json_bool(cruise.getCancel()) + ",";
  s += "\"resume\":" + json_bool(cruise.getResume()) + ",";
  s += "\"override\":" + json_bool(cruise.getOverride()) + "},";

  s += "\"hudControl\":{";
  s += "\"speedVisible\":" + json_bool(hud.getSpeedVisible()) + ",";
  s += "\"setSpeed\":" + json_num(hud.getSetSpeed()) + ",";
  s += "\"lanesVisible\":" + json_bool(hud.getLanesVisible()) + ",";
  s += "\"leadVisible\":" + json_bool(hud.getLeadVisible()) + ",";
  s += "\"visualAlert\":" + json_num(static_cast<int>(hud.getVisualAlert())) + ",";
  s += "\"rightLaneVisible\":" + json_bool(hud.getRightLaneVisible()) + ",";
  s += "\"leftLaneVisible\":" + json_bool(hud.getLeftLaneVisible()) + ",";
  s += "\"rightLaneDepart\":" + json_bool(hud.getRightLaneDepart()) + ",";
  s += "\"leftLaneDepart\":" + json_bool(hud.getLeftLaneDepart()) + ",";
  s += "\"leadDistanceBars\":" + json_num(static_cast<int>(hud.getLeadDistanceBars())) + ",";
  s += "\"audibleAlert\":" + json_num(static_cast<int>(hud.getAudibleAlert())) + "},";

  s += "\"logMonoTime\":" + json_num(log_mono_time);
  s += "}}";
  return s;
}

std::string build_car_output_json(cereal::CarOutput::Reader co, uint64_t log_mono_time) {
  std::string s = "{\"ts\":" + unix_ts_json() +
                  ",\"type\":\"carOutput\",\"data\":{";
  s += "\"actuatorsOutput\":" + build_car_control_actuators_json(co.getActuatorsOutput()) + ",";
  s += "\"logMonoTime\":" + json_num(log_mono_time);
  s += "}}";
  return s;
}

std::string build_live_parameters_json(cereal::LiveParametersData::Reader lp, uint64_t log_mono_time) {
  std::string s = "{\"ts\":" + unix_ts_json() +
                  ",\"type\":\"liveParameters\",\"data\":{";
  s += "\"valid\":" + json_bool(lp.getValid()) + ",";
  s += "\"sensorValid\":" + json_bool(lp.getSensorValid()) + ",";
  s += "\"posenetValid\":" + json_bool(lp.getPosenetValid()) + ",";
  s += "\"gyroBias\":" + json_num(lp.getGyroBias()) + ",";
  s += "\"angleOffsetDeg\":" + json_num(lp.getAngleOffsetDeg()) + ",";
  s += "\"angleOffsetAverageDeg\":" + json_num(lp.getAngleOffsetAverageDeg()) + ",";
  s += "\"stiffnessFactor\":" + json_num(lp.getStiffnessFactor()) + ",";
  s += "\"steerRatio\":" + json_num(lp.getSteerRatio()) + ",";
  s += "\"roll\":" + json_num(lp.getRoll()) + ",";
  s += "\"posenetSpeed\":" + json_num(lp.getPosenetSpeed()) + ",";
  s += "\"angleOffsetFastStd\":" + json_num(lp.getAngleOffsetFastStd()) + ",";
  s += "\"angleOffsetAverageStd\":" + json_num(lp.getAngleOffsetAverageStd()) + ",";
  s += "\"stiffnessFactorStd\":" + json_num(lp.getStiffnessFactorStd()) + ",";
  s += "\"steerRatioStd\":" + json_num(lp.getSteerRatioStd()) + ",";
  s += "\"angleOffsetValid\":" + json_bool(lp.getAngleOffsetValid()) + ",";
  s += "\"angleOffsetAverageValid\":" + json_bool(lp.getAngleOffsetAverageValid()) + ",";
  s += "\"steerRatioValid\":" + json_bool(lp.getSteerRatioValid()) + ",";
  s += "\"stiffnessFactorValid\":" + json_bool(lp.getStiffnessFactorValid()) + ",";
  s += "\"debugFilterState\":{";
  s += "\"value\":" + json_float_array(lp.getDebugFilterState().getValue()) + ",";
  s += "\"std\":" + json_float_array(lp.getDebugFilterState().getStd()) + "},";
  s += "\"logMonoTime\":" + json_num(log_mono_time);
  s += "}}";
  return s;
}

std::string build_onroad_event_json(cereal::OnroadEvent::Reader e) {
  std::string s = "{";
  s += "\"name\":" + json_num(static_cast<int>(e.getName())) + ",";
  s += "\"enable\":" + json_bool(e.getEnable()) + ",";
  s += "\"noEntry\":" + json_bool(e.getNoEntry()) + ",";
  s += "\"warning\":" + json_bool(e.getWarning()) + ",";
  s += "\"userDisable\":" + json_bool(e.getUserDisable()) + ",";
  s += "\"softDisable\":" + json_bool(e.getSoftDisable()) + ",";
  s += "\"immediateDisable\":" + json_bool(e.getImmediateDisable()) + ",";
  s += "\"preEnable\":" + json_bool(e.getPreEnable()) + ",";
  s += "\"permanent\":" + json_bool(e.getPermanent()) + ",";
  s += "\"overrideLateral\":" + json_bool(e.getOverrideLateral()) + ",";
  s += "\"overrideLongitudinal\":" + json_bool(e.getOverrideLongitudinal());
  s += "}";
  return s;
}

std::string build_onroad_events_json(capnp::List<cereal::OnroadEvent>::Reader events, uint64_t log_mono_time) {
  std::string s = "{\"ts\":" + unix_ts_json() +
                  ",\"type\":\"onroadEvents\",\"data\":{";
  s += "\"count\":" + json_num(events.size()) + ",";
  s += "\"events\":[";
  for (unsigned i = 0; i < events.size(); ++i) {
    if (i > 0) s += ",";
    s += build_onroad_event_json(events[i]);
  }
  s += "],";
  s += "\"logMonoTime\":" + json_num(log_mono_time);
  s += "}}";
  return s;
}

std::string build_driver_monitoring_state_json(cereal::DriverMonitoringState::Reader dm,
                                               uint64_t log_mono_time) {
  auto events = dm.getEvents();
  std::string s = "{\"ts\":" + unix_ts_json() +
                  ",\"type\":\"driverMonitoringState\",\"data\":{";
  s += "\"faceDetected\":" + json_bool(dm.getFaceDetected()) + ",";
  s += "\"isDistracted\":" + json_bool(dm.getIsDistracted()) + ",";
  s += "\"distractedType\":" + json_num(dm.getDistractedType()) + ",";
  s += "\"awarenessStatus\":" + json_num(dm.getAwarenessStatus()) + ",";
  s += "\"awarenessActive\":" + json_num(dm.getAwarenessActive()) + ",";
  s += "\"awarenessPassive\":" + json_num(dm.getAwarenessPassive()) + ",";
  s += "\"stepChange\":" + json_num(dm.getStepChange()) + ",";
  s += "\"posePitchOffset\":" + json_num(dm.getPosePitchOffset()) + ",";
  s += "\"posePitchValidCount\":" + json_num(dm.getPosePitchValidCount()) + ",";
  s += "\"poseYawOffset\":" + json_num(dm.getPoseYawOffset()) + ",";
  s += "\"poseYawValidCount\":" + json_num(dm.getPoseYawValidCount()) + ",";
  s += "\"isLowStd\":" + json_bool(dm.getIsLowStd()) + ",";
  s += "\"hiStdCount\":" + json_num(dm.getHiStdCount()) + ",";
  s += "\"isActiveMode\":" + json_bool(dm.getIsActiveMode()) + ",";
  s += "\"isRHD\":" + json_bool(dm.getIsRHD()) + ",";
  s += "\"uncertainCount\":" + json_num(dm.getUncertainCount()) + ",";
  s += "\"events\":[";
  for (unsigned i = 0; i < events.size(); ++i) {
    if (i > 0) s += ",";
    s += build_onroad_event_json(events[i]);
  }
  s += "],";
  s += "\"logMonoTime\":" + json_num(log_mono_time);
  s += "}}";
  return s;
}

std::string build_driver_data_json(cereal::DriverStateV2::DriverData::Reader d) {
  std::string s = "{";
  s += "\"faceOrientation\":" + json_float_array(d.getFaceOrientation()) + ",";
  s += "\"faceOrientationStd\":" + json_float_array(d.getFaceOrientationStd()) + ",";
  s += "\"facePosition\":" + json_float_array(d.getFacePosition()) + ",";
  s += "\"facePositionStd\":" + json_float_array(d.getFacePositionStd()) + ",";
  s += "\"faceProb\":" + json_num(d.getFaceProb()) + ",";
  s += "\"leftEyeProb\":" + json_num(d.getLeftEyeProb()) + ",";
  s += "\"rightEyeProb\":" + json_num(d.getRightEyeProb()) + ",";
  s += "\"leftBlinkProb\":" + json_num(d.getLeftBlinkProb()) + ",";
  s += "\"rightBlinkProb\":" + json_num(d.getRightBlinkProb()) + ",";
  s += "\"sunglassesProb\":" + json_num(d.getSunglassesProb()) + ",";
  s += "\"phoneProb\":" + json_num(d.getPhoneProb());
  s += "}";
  return s;
}

std::string build_driver_state_v2_json(cereal::DriverStateV2::Reader d, uint64_t log_mono_time) {
  std::string s = "{\"ts\":" + unix_ts_json() +
                  ",\"type\":\"driverStateV2\",\"data\":{";
  s += "\"frameId\":" + json_num(d.getFrameId()) + ",";
  s += "\"modelExecutionTime\":" + json_num(d.getModelExecutionTime()) + ",";
  s += "\"gpuExecutionTime\":" + json_num(d.getGpuExecutionTime()) + ",";
  s += "\"wheelOnRightProb\":" + json_num(d.getWheelOnRightProb()) + ",";
  s += "\"leftDriverData\":" + build_driver_data_json(d.getLeftDriverData()) + ",";
  s += "\"rightDriverData\":" + build_driver_data_json(d.getRightDriverData()) + ",";
  s += "\"logMonoTime\":" + json_num(log_mono_time);
  s += "}}";
  return s;
}

std::string build_road_camera_state_json(cereal::FrameData::Reader f, uint64_t log_mono_time) {
  std::string s = "{\"ts\":" + unix_ts_json() +
                  ",\"type\":\"roadCameraState\",\"data\":{";
  s += "\"frameId\":" + json_num(f.getFrameId()) + ",";
  s += "\"frameIdSensor\":" + json_num(f.getFrameIdSensor()) + ",";
  s += "\"requestId\":" + json_num(f.getRequestId()) + ",";
  s += "\"encodeId\":" + json_num(f.getEncodeId()) + ",";
  s += "\"timestampEof\":" + json_num(f.getTimestampEof()) + ",";
  s += "\"timestampSof\":" + json_num(f.getTimestampSof()) + ",";
  s += "\"processingTime\":" + json_num(f.getProcessingTime()) + ",";
  s += "\"integLines\":" + json_num(f.getIntegLines()) + ",";
  s += "\"gain\":" + json_num(f.getGain()) + ",";
  s += "\"highConversionGain\":" + json_bool(f.getHighConversionGain()) + ",";
  s += "\"measuredGreyFraction\":" + json_num(f.getMeasuredGreyFraction()) + ",";
  s += "\"targetGreyFraction\":" + json_num(f.getTargetGreyFraction()) + ",";
  s += "\"exposureValPercent\":" + json_num(f.getExposureValPercent()) + ",";
  s += "\"temperaturesC\":" + json_float_array(f.getTemperaturesC()) + ",";
  s += "\"sensor\":" + json_num(static_cast<int>(f.getSensor())) + ",";
  s += "\"logMonoTime\":" + json_num(log_mono_time);
  s += "}}";
  return s;
}

}  // namespace

std::string build_telemetry_json(cereal::Event::Reader event) {
  const uint64_t log_mono_time = event.getLogMonoTime();
  switch (event.which()) {
    case cereal::Event::CAR_STATE:
      return build_car_state_json(event.getCarState(), log_mono_time);
    case cereal::Event::SELFDRIVE_STATE:
      return build_selfdrive_state_json(event.getSelfdriveState(), log_mono_time);
    case cereal::Event::DEVICE_STATE:
      return build_device_state_json(event.getDeviceState());
    case cereal::Event::MODEL_V2:
      return build_model_v2_json(event.getModelV2(), log_mono_time);
    case cereal::Event::RADAR_STATE:
      return build_radar_state_json(event.getRadarState(), log_mono_time);
    case cereal::Event::LIVE_CALIBRATION:
      return build_live_calibration_json(event.getLiveCalibration(), log_mono_time);
    case cereal::Event::CAR_CONTROL:
      return build_car_control_json(event.getCarControl(), log_mono_time);
    case cereal::Event::CAR_OUTPUT:
      return build_car_output_json(event.getCarOutput(), log_mono_time);
    case cereal::Event::LIVE_PARAMETERS:
      return build_live_parameters_json(event.getLiveParameters(), log_mono_time);
    case cereal::Event::DRIVER_MONITORING_STATE:
      return build_driver_monitoring_state_json(event.getDriverMonitoringState(), log_mono_time);
    case cereal::Event::DRIVER_STATE_V2:
      return build_driver_state_v2_json(event.getDriverStateV2(), log_mono_time);
    case cereal::Event::ONROAD_EVENTS:
      return build_onroad_events_json(event.getOnroadEvents(), log_mono_time);
    case cereal::Event::ROAD_CAMERA_STATE:
      return build_road_camera_state_json(event.getRoadCameraState(), log_mono_time);
    default:
      return std::string();
  }
}

}  // namespace commaview::telemetry
