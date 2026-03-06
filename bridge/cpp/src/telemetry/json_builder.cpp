#include "commaview/telemetry/json_builder.h"

#include <cstdio>
#include <ctime>

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

template<typename T>
std::string json_float_array(T list, int max_count = -1) {
  std::string s = "[";
  int i = 0;
  for (auto v : list) {
    if (max_count >= 0 && i >= max_count) break;
    if (i > 0) s += ",";
    char buf[32];
    snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
    s += buf;
    i++;
  }
  s += "]";
  return s;
}

std::string build_car_state_json(cereal::CarState::Reader cs) {
  char buf[256];
  snprintf(buf, sizeof(buf),
      "{\"ts\":%.3f,\"type\":\"carState\",\"data\":"
      "{\"speed\":%.6g,\"steeringAngle\":%.6g,"
      "\"brakePressed\":%s,\"gasPressed\":%s}}",
      static_cast<double>(time(nullptr)), static_cast<double>(cs.getVEgo()),
      static_cast<double>(cs.getSteeringAngleDeg()),
      cs.getBrakePressed() ? "true" : "false",
      cs.getGasPressed() ? "true" : "false");
  return std::string(buf);
}

std::string build_selfdrive_state_json(cereal::SelfdriveState::Reader ss) {
  char buf[512];
  snprintf(buf, sizeof(buf),
      "{\"ts\":%.3f,\"type\":\"controlsState\",\"data\":"
      "{\"enabled\":%s,\"alertText1\":\"%s\","
      "\"alertText2\":\"%s\",\"alertType\":\"%s\"}}",
      static_cast<double>(time(nullptr)),
      ss.getEnabled() ? "true" : "false",
      json_escape(ss.getAlertText1().cStr()).c_str(),
      json_escape(ss.getAlertText2().cStr()).c_str(),
      json_escape(ss.getAlertType().cStr()).c_str());
  return std::string(buf);
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
  std::string s = "{\"ts\":" + std::to_string(static_cast<double>(time(nullptr))) +
                  ",\"type\":\"modelV2\",\"data\":{";

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
         ",\"prob\":" + std::to_string(static_cast<double>(prob)) +
         ",\"std\":" + std::to_string(static_cast<double>(std)) + "}";
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
         ",\"std\":" + std::to_string(static_cast<double>(std)) + "}";
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
         ",\"prob\":" + std::to_string(static_cast<double>(leads[i].getProb())) + "}";
  }
  s += "],";

  s += "\"laneLineStds\":" + json_float_array(lane_stds) + ",";
  s += "\"roadEdgeStds\":" + json_float_array(edge_stds) + ",";
  s += "\"logMonoTime\":" + std::to_string(log_mono_time);
  s += "}}";

  return s;
}

std::string build_radar_state_json(cereal::RadarState::Reader r, uint64_t log_mono_time) {
  auto lead_json = [](cereal::RadarState::LeadData::Reader l) {
    std::string j = "{\"dRel\":" + std::to_string(static_cast<double>(l.getDRel())) +
                    ",\"yRel\":" + std::to_string(static_cast<double>(l.getYRel())) +
                    ",\"vRel\":" + std::to_string(static_cast<double>(l.getVRel())) +
                    ",\"aRel\":" + std::to_string(static_cast<double>(l.getARel())) +
                    ",\"status\":" + std::string(l.getStatus() ? "true" : "false") + "}";
    return j;
  };

  auto l1 = r.getLeadOne();
  auto l2 = r.getLeadTwo();

  std::string s = "{\"ts\":" + std::to_string(static_cast<double>(time(nullptr))) +
                  ",\"type\":\"radarState\",\"data\":{";
  s += "\"leadOne\":" + lead_json(l1) + ",";
  s += "\"leadTwo\":" + lead_json(l2) + ",";
  s += "\"logMonoTime\":" + std::to_string(log_mono_time);
  s += "}}";
  return s;
}

std::string build_live_calibration_json(cereal::LiveCalibrationData::Reader c, uint64_t log_mono_time) {
  std::string s = "{\"ts\":" + std::to_string(static_cast<double>(time(nullptr))) +
                  ",\"type\":\"liveCalibration\",\"data\":{";
  s += "\"rpyCalib\":" + json_float_array(c.getRpyCalib()) + ",";
  s += "\"height\":" + json_float_array(c.getHeight()) + ",";
  s += "\"calStatus\":\"" + std::to_string(static_cast<int>(c.getCalStatus())) + "\",";
  s += "\"calStatusInt\":" + std::to_string(static_cast<int>(c.getCalStatus())) + ",";
  s += "\"calPerc\":" + std::to_string(c.getCalPerc()) + ",";
  s += "\"logMonoTime\":" + std::to_string(log_mono_time) + "}}";
  return s;
}

}  // namespace

std::string build_telemetry_json(cereal::Event::Reader event) {
  const uint64_t log_mono_time = event.getLogMonoTime();
  switch (event.which()) {
    case cereal::Event::CAR_STATE:
      return build_car_state_json(event.getCarState());
    case cereal::Event::SELFDRIVE_STATE:
      return build_selfdrive_state_json(event.getSelfdriveState());
    case cereal::Event::DEVICE_STATE:
      return build_device_state_json(event.getDeviceState());
    case cereal::Event::MODEL_V2:
      return build_model_v2_json(event.getModelV2(), log_mono_time);
    case cereal::Event::RADAR_STATE:
      return build_radar_state_json(event.getRadarState(), log_mono_time);
    case cereal::Event::LIVE_CALIBRATION:
      return build_live_calibration_json(event.getLiveCalibration(), log_mono_time);
    default:
      return std::string();
  }
}

}  // namespace commaview::telemetry
