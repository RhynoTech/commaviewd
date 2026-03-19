#include "framing.h"
#include "socket.h"
#include "policy.h"
#include "router.h"
#include "json_builder.h"
#include "telemetry_stats.h"
/**
 * CommaView Unified Bridge (C++)
 *
 * Streams HEVC video on all ports and telemetry JSON on road+wide ports.
 *
 * Ports:
 *   8200 road, 8201 wide, 8202 driver
 *
 * Framing:
 *   [4-byte big-endian length][payload]
 *   payload[0] = 0x01 (video): [type][header_len_be32][header][data]
 *   payload[0] = 0x02 (meta):  [type][json bytes]
 *   payload[0] = 0x03 (control inbound): [type][json bytes]
 *   payload[0] = 0x04 (meta-raw): [type][version][service_idx][event_which_be16][log_mono_be64][raw_len_be32][raw_event]
 */

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <atomic>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <fstream>

#include <capnp/serialize.h>
#include "cereal/gen/cpp/log.capnp.h"
#include "cereal/messaging/messaging.h"
#include "cereal/services.h"

static constexpr uint8_t MSG_VIDEO = 0x01;
static constexpr uint8_t MSG_META  = 0x02;
static constexpr uint8_t MSG_CONTROL = 0x03;
static constexpr uint8_t MSG_META_RAW = 0x04;
static constexpr uint8_t META_RAW_VERSION = 0x01;

static constexpr int PORT_ROAD = 8200;
static constexpr int PORT_WIDE = 8201;
static constexpr int PORT_DRIVER = 8202;

// Parity profile: onroad-critical telemetry set, emitted at coalesced 20 Hz.
static const char* TELEMETRY_SERVICES[] = {
  "carState", "selfdriveState", "deviceState", "liveCalibration", "radarState", "modelV2",
  "carControl", "carOutput", "liveParameters", "driverMonitoringState",
  "driverStateV2", "onroadEvents", "roadCameraState"
};
static constexpr int NUM_TELEM = 13;
static constexpr int TELEMETRY_EMIT_MS_DEFAULT = 50;  // 20 Hz parity target
static int g_telemetry_emit_ms = TELEMETRY_EMIT_MS_DEFAULT;

static constexpr uint32_t TELEMETRY_BIT_CARSTATE = 1u << 0;
static constexpr uint32_t TELEMETRY_BIT_SELFDRIVE = 1u << 1;
static constexpr uint32_t TELEMETRY_BIT_DEVICESTATE = 1u << 2;
static constexpr uint32_t TELEMETRY_BIT_LIVECALIB = 1u << 3;
static constexpr uint32_t TELEMETRY_BIT_RADAR = 1u << 4;
static constexpr uint32_t TELEMETRY_BIT_MODELV2 = 1u << 5;
static constexpr uint32_t TELEMETRY_BIT_CARCONTROL = 1u << 6;
static constexpr uint32_t TELEMETRY_BIT_CAROUTPUT = 1u << 7;
static constexpr uint32_t TELEMETRY_BIT_LIVEPARAMS = 1u << 8;
static constexpr uint32_t TELEMETRY_BIT_DRIVERMON = 1u << 9;
static constexpr uint32_t TELEMETRY_BIT_DRIVERV2 = 1u << 10;
static constexpr uint32_t TELEMETRY_BIT_ONROADEVENTS = 1u << 11;
static constexpr uint32_t TELEMETRY_BIT_ROADCAMSTATE = 1u << 12;
static constexpr uint32_t TELEMETRY_MASK_ALL =
    TELEMETRY_BIT_CARSTATE |
    TELEMETRY_BIT_SELFDRIVE |
    TELEMETRY_BIT_DEVICESTATE |
    TELEMETRY_BIT_LIVECALIB |
    TELEMETRY_BIT_RADAR |
    TELEMETRY_BIT_MODELV2 |
    TELEMETRY_BIT_CARCONTROL |
    TELEMETRY_BIT_CAROUTPUT |
    TELEMETRY_BIT_LIVEPARAMS |
    TELEMETRY_BIT_DRIVERMON |
    TELEMETRY_BIT_DRIVERV2 |
    TELEMETRY_BIT_ONROADEVENTS |
    TELEMETRY_BIT_ROADCAMSTATE;
static constexpr uint32_t TELEMETRY_MASK_SAFE_NO_CAR =
    TELEMETRY_MASK_ALL & ~TELEMETRY_BIT_CARSTATE;

static const char* VIDEO_SERVICES_PROD[] = {
  "roadEncodeData", "wideRoadEncodeData", "driverEncodeData"
};

static const char* VIDEO_SERVICES_DEV[] = {
  "livestreamRoadEncodeData", "livestreamWideRoadEncodeData", "livestreamDriverEncodeData"
};

static bool g_dev_mode = false;
static bool g_video_only = false;
static bool g_suppress_video = false;
static bool g_telemetry_only = false;
static bool g_telemetry_blackhole = false;
static bool g_telemetry_drain_only = false;
static bool g_telemetry_subscribe_only = false;
static bool g_emit_meta_json = false;
static bool g_emit_meta_raw = true;
static uint32_t g_telemetry_mask = TELEMETRY_MASK_ALL;
static std::atomic<bool> g_running{true};


static std::atomic<int> g_active_road{0};
static std::atomic<int> g_active_wide{0};
static std::atomic<int> g_active_driver{0};

static std::atomic<int>& active_counter_for_port(int port) {
  if (port == PORT_ROAD) return g_active_road;
  if (port == PORT_WIDE) return g_active_wide;
  return g_active_driver;
}

static cereal::Event::Which expected_video_which_for_port(int port) {
  return commaview::video::expected_video_which_for_port(port, g_dev_mode);
}

static bool telemetry_enabled_index(int i) {
  switch (i) {
    case 0: return (g_telemetry_mask & TELEMETRY_BIT_CARSTATE) != 0;
    case 1: return (g_telemetry_mask & TELEMETRY_BIT_SELFDRIVE) != 0;
    case 2: return (g_telemetry_mask & TELEMETRY_BIT_DEVICESTATE) != 0;
    case 3: return (g_telemetry_mask & TELEMETRY_BIT_LIVECALIB) != 0;
    case 4: return (g_telemetry_mask & TELEMETRY_BIT_RADAR) != 0;
    case 5: return (g_telemetry_mask & TELEMETRY_BIT_MODELV2) != 0;
    case 6: return (g_telemetry_mask & TELEMETRY_BIT_CARCONTROL) != 0;
    case 7: return (g_telemetry_mask & TELEMETRY_BIT_CAROUTPUT) != 0;
    case 8: return (g_telemetry_mask & TELEMETRY_BIT_LIVEPARAMS) != 0;
    case 9: return (g_telemetry_mask & TELEMETRY_BIT_DRIVERMON) != 0;
    case 10: return (g_telemetry_mask & TELEMETRY_BIT_DRIVERV2) != 0;
    case 11: return (g_telemetry_mask & TELEMETRY_BIT_ONROADEVENTS) != 0;
    case 12: return (g_telemetry_mask & TELEMETRY_BIT_ROADCAMSTATE) != 0;
    default: return false;
  }
}

static int clamp_telem_emit_ms(int value) {
  if (value < 10) return 10;
  if (value > 1000) return 1000;
  return value;
}

static int parse_int_or_default(const char* text, int fallback) {
  if (text == nullptr || *text == '\0') return fallback;
  char* end = nullptr;
  long v = strtol(text, &end, 10);
  if (end == text || (end && *end != '\0')) return fallback;
  return static_cast<int>(v);
}

static const char* telemetry_mask_label() {
  if (g_telemetry_mask == TELEMETRY_MASK_ALL) return "all";
  if (g_telemetry_mask == TELEMETRY_BIT_CARSTATE) return "carState";
  if (g_telemetry_mask == TELEMETRY_BIT_SELFDRIVE) return "selfdriveState";
  if (g_telemetry_mask == TELEMETRY_BIT_DEVICESTATE) return "deviceState";
  if (g_telemetry_mask == TELEMETRY_BIT_LIVECALIB) return "liveCalibration";
  if (g_telemetry_mask == TELEMETRY_BIT_RADAR) return "radarState";
  if (g_telemetry_mask == TELEMETRY_BIT_MODELV2) return "modelV2";
  if (g_telemetry_mask == TELEMETRY_BIT_CARCONTROL) return "carControl";
  if (g_telemetry_mask == TELEMETRY_BIT_CAROUTPUT) return "carOutput";
  if (g_telemetry_mask == TELEMETRY_BIT_LIVEPARAMS) return "liveParameters";
  if (g_telemetry_mask == TELEMETRY_BIT_DRIVERMON) return "driverMonitoringState";
  if (g_telemetry_mask == TELEMETRY_BIT_DRIVERV2) return "driverStateV2";
  if (g_telemetry_mask == TELEMETRY_BIT_ONROADEVENTS) return "onroadEvents";
  if (g_telemetry_mask == TELEMETRY_BIT_ROADCAMSTATE) return "roadCameraState";
  if (g_telemetry_mask == TELEMETRY_MASK_SAFE_NO_CAR) return "safeNoCar";
  return "custom";
}
static const char* telemetry_mode_label() {
  if (g_emit_meta_json && g_emit_meta_raw) return "raw+json";
  if (g_emit_meta_json) return "json-only";
  if (g_emit_meta_raw) return "raw-only";
  return "none";
}


static size_t queue_size_for_service(const char* service_name) {
  auto it = services.find(std::string(service_name));
  if (it == services.end()) return 0;
  return it->second.queue_size;
}

static void put_be32(uint8_t* buf, uint32_t val) {
  commaview::net::put_be32(buf, val);
}

static uint32_t read_be32(const uint8_t* buf) {
  return commaview::net::read_be32(buf);
}

static void put_be16(uint8_t* buf, uint16_t val) {
  buf[0] = (val >> 8) & 0xFF;
  buf[1] = val & 0xFF;
}

static void put_be64(uint8_t* buf, uint64_t val) {
  buf[0] = (val >> 56) & 0xFF;
  buf[1] = (val >> 48) & 0xFF;
  buf[2] = (val >> 40) & 0xFF;
  buf[3] = (val >> 32) & 0xFF;
  buf[4] = (val >> 24) & 0xFF;
  buf[5] = (val >> 16) & 0xFF;
  buf[6] = (val >> 8) & 0xFF;
  buf[7] = val & 0xFF;
}

static int telemetry_index_for_which(cereal::Event::Which which) {
  switch (which) {
    case cereal::Event::CAR_STATE: return 0;
    case cereal::Event::SELFDRIVE_STATE: return 1;
    case cereal::Event::DEVICE_STATE: return 2;
    case cereal::Event::LIVE_CALIBRATION: return 3;
    case cereal::Event::RADAR_STATE: return 4;
    case cereal::Event::MODEL_V2: return 5;
    case cereal::Event::CAR_CONTROL: return 6;
    case cereal::Event::CAR_OUTPUT: return 7;
    case cereal::Event::LIVE_PARAMETERS: return 8;
    case cereal::Event::DRIVER_MONITORING_STATE: return 9;
    case cereal::Event::DRIVER_STATE_V2: return 10;
    case cereal::Event::ONROAD_EVENTS: return 11;
    case cereal::Event::ROAD_CAMERA_STATE: return 12;
    default: return -1;
  }
}


static void push_u8(std::vector<uint8_t>& out, uint8_t v) {
  out.push_back(v);
}

static void push_u16(std::vector<uint8_t>& out, uint16_t v) {
  uint8_t b[2];
  put_be16(b, v);
  out.insert(out.end(), b, b + 2);
}

static void push_u32(std::vector<uint8_t>& out, uint32_t v) {
  uint8_t b[4];
  put_be32(b, v);
  out.insert(out.end(), b, b + 4);
}

static void push_u64(std::vector<uint8_t>& out, uint64_t v) {
  uint8_t b[8];
  put_be64(b, v);
  out.insert(out.end(), b, b + 8);
}

static void push_f32(std::vector<uint8_t>& out, float value) {
  uint32_t bits = 0;
  static_assert(sizeof(float) == sizeof(uint32_t), "float size");
  memcpy(&bits, &value, sizeof(bits));
  push_u32(out, bits);
}

static void push_str_u16(std::vector<uint8_t>& out, kj::StringPtr s) {
  const auto n = static_cast<uint16_t>(std::min<size_t>(s.size(), 65535));
  push_u16(out, n);
  out.insert(out.end(), s.begin(), s.begin() + n);
}

static std::vector<uint8_t> encode_car_state_typed(cereal::CarState::Reader cs) {
  std::vector<uint8_t> out;
  out.reserve(1 + 4 * 5 + 4);
  push_u8(out, 0x01);  // schema version
  push_f32(out, static_cast<float>(cs.getVEgo()));
  push_f32(out, static_cast<float>(cs.getSteeringAngleDeg()));
  push_u8(out, cs.getBrakePressed() ? 1 : 0);
  push_u8(out, cs.getGasPressed() ? 1 : 0);
  push_f32(out, static_cast<float>(cs.getAEgo()));
  push_f32(out, static_cast<float>(cs.getSteeringTorqueEps()));
  push_u8(out, cs.getSteeringPressed() ? 1 : 0);
  push_u8(out, cs.getCanValid() ? 1 : 0);
  push_u8(out, cs.getCanTimeout() ? 1 : 0);
  push_u32(out, static_cast<uint32_t>(cs.getCanErrorCounter()));
  return out;
}

static std::vector<uint8_t> encode_selfdrive_state_typed(cereal::SelfdriveState::Reader ss) {
  std::vector<uint8_t> out;
  out.reserve(64);
  push_u8(out, 0x01);  // schema version
  push_u8(out, ss.getEnabled() ? 1 : 0);
  push_u8(out, ss.getActive() ? 1 : 0);
  push_u8(out, ss.getEngageable() ? 1 : 0);
  push_u8(out, ss.getExperimentalMode() ? 1 : 0);
  push_u32(out, static_cast<uint32_t>(ss.getState()));
  push_u32(out, static_cast<uint32_t>(ss.getAlertStatus()));
  push_u32(out, static_cast<uint32_t>(ss.getAlertSize()));
  push_u32(out, static_cast<uint32_t>(ss.getAlertSound()));
  push_u32(out, static_cast<uint32_t>(ss.getAlertHudVisual()));
  push_u32(out, static_cast<uint32_t>(ss.getPersonality()));
  push_str_u16(out, ss.getAlertText1());
  push_str_u16(out, ss.getAlertText2());
  push_str_u16(out, ss.getAlertType());
  return out;
}


static std::vector<uint8_t> encode_car_control_typed(cereal::CarControl::Reader cc) {
  std::vector<uint8_t> out;
  out.reserve(128);
  const auto act = cc.getActuators();
  const auto cruise = cc.getCruiseControl();
  const auto hud = cc.getHudControl();

  push_u8(out, 0x01);  // schema version
  push_u8(out, cc.getEnabled() ? 1 : 0);
  push_u8(out, cc.getLatActive() ? 1 : 0);
  push_u8(out, cc.getLongActive() ? 1 : 0);
  push_u8(out, cc.getLeftBlinker() ? 1 : 0);
  push_u8(out, cc.getRightBlinker() ? 1 : 0);
  push_f32(out, static_cast<float>(cc.getCurrentCurvature()));

  push_f32(out, static_cast<float>(act.getTorque()));
  push_f32(out, static_cast<float>(act.getSteeringAngleDeg()));
  push_f32(out, static_cast<float>(act.getCurvature()));
  push_f32(out, static_cast<float>(act.getAccel()));
  push_u32(out, static_cast<uint32_t>(act.getLongControlState()));
  push_f32(out, static_cast<float>(act.getGas()));
  push_f32(out, static_cast<float>(act.getBrake()));
  push_f32(out, static_cast<float>(act.getTorqueOutputCan()));
  push_f32(out, static_cast<float>(act.getSpeed()));

  push_u8(out, cruise.getCancel() ? 1 : 0);
  push_u8(out, cruise.getResume() ? 1 : 0);
  push_u8(out, cruise.getOverride() ? 1 : 0);

  push_u8(out, hud.getSpeedVisible() ? 1 : 0);
  push_f32(out, static_cast<float>(hud.getSetSpeed()));
  push_u8(out, hud.getLanesVisible() ? 1 : 0);
  push_u8(out, hud.getLeadVisible() ? 1 : 0);
  push_u32(out, static_cast<uint32_t>(hud.getVisualAlert()));
  push_u8(out, hud.getRightLaneVisible() ? 1 : 0);
  push_u8(out, hud.getLeftLaneVisible() ? 1 : 0);
  push_u8(out, hud.getRightLaneDepart() ? 1 : 0);
  push_u8(out, hud.getLeftLaneDepart() ? 1 : 0);
  push_u32(out, static_cast<uint32_t>(hud.getLeadDistanceBars()));
  push_u32(out, static_cast<uint32_t>(hud.getAudibleAlert()));
  return out;
}



static void push_float_list(std::vector<uint8_t>& out, capnp::List<float>::Reader vals) {
  push_u32(out, static_cast<uint32_t>(vals.size()));
  for (auto v : vals) push_f32(out, static_cast<float>(v));
}

static std::vector<uint8_t> encode_device_state_typed(cereal::DeviceState::Reader ds) {
  std::vector<uint8_t> out;
  out.reserve(64);
  auto cpu = ds.getCpuTempC();

  auto gpu = ds.getGpuTempC();
  const float cpu0 = cpu.size() > 0 ? static_cast<float>(cpu[0]) : 0.0f;
  const float gpu0 = gpu.size() > 0 ? static_cast<float>(gpu[0]) : 0.0f;

  push_u8(out, 0x01);  // schema version
  push_f32(out, cpu0);
  push_f32(out, gpu0);
  push_u32(out, static_cast<uint32_t>(ds.getMemoryUsagePercent()));
  push_f32(out, static_cast<float>(ds.getFreeSpacePercent()));
  push_u32(out, static_cast<uint32_t>(ds.getNetworkStrength()));
  push_u32(out, static_cast<uint32_t>(ds.getThermalStatus()));
  push_u8(out, ds.getStarted() ? 1 : 0);
  return out;
}

static std::vector<uint8_t> encode_radar_state_typed(cereal::RadarState::Reader rs) {
  std::vector<uint8_t> out;
  out.reserve(64);
  auto l1 = rs.getLeadOne();
  auto l2 = rs.getLeadTwo();

  push_u8(out, 0x01);  // schema version
  auto push_lead = [&](cereal::RadarState::LeadData::Reader l) {
    push_f32(out, static_cast<float>(l.getDRel()));
    push_f32(out, static_cast<float>(l.getYRel()));
    push_f32(out, static_cast<float>(l.getVRel()));
    push_f32(out, static_cast<float>(l.getARel()));
    push_u8(out, l.getStatus() ? 1 : 0);
  };
  push_lead(l1);
  push_lead(l2);
  return out;
}


static std::vector<uint8_t> encode_model_v2_typed(cereal::ModelDataV2::Reader m) {
  std::vector<uint8_t> out;
  out.reserve(4096);

  auto push_xyz = [&](cereal::XYZTData::Reader xyz) {
    push_float_list(out, xyz.getX());
    push_float_list(out, xyz.getY());
    push_float_list(out, xyz.getZ());
  };

  push_u8(out, 0x01);  // schema version
  push_u64(out, static_cast<uint64_t>(m.getFrameId()));
  push_u64(out, static_cast<uint64_t>(m.getFrameIdExtra()));
  push_u64(out, static_cast<uint64_t>(m.getFrameAge()));
  push_f32(out, static_cast<float>(m.getFrameDropPerc()));
  push_u64(out, static_cast<uint64_t>(m.getTimestampEof()));

  auto lanes = m.getLaneLines();
  auto lane_probs = m.getLaneLineProbs();
  auto lane_stds = m.getLaneLineStds();
  push_u32(out, static_cast<uint32_t>(lanes.size()));
  for (unsigned i = 0; i < lanes.size(); i++) {
    push_xyz(lanes[i]);
    const float prob = (i < lane_probs.size()) ? lane_probs[i] : 0.0f;
    const float std = (i < lane_stds.size()) ? lane_stds[i] : 0.0f;
    push_f32(out, prob);
    push_f32(out, std);
  }

  auto edges = m.getRoadEdges();
  auto edge_stds = m.getRoadEdgeStds();
  push_u32(out, static_cast<uint32_t>(edges.size()));
  for (unsigned i = 0; i < edges.size(); i++) {
    push_xyz(edges[i]);
    const float std = (i < edge_stds.size()) ? edge_stds[i] : 0.0f;
    push_f32(out, std);
  }

  auto pos = m.getPosition();
  push_float_list(out, pos.getX());
  push_float_list(out, pos.getY());
  push_float_list(out, pos.getZ());

  auto leads = m.getLeadsV3();
  push_u32(out, static_cast<uint32_t>(leads.size()));
  for (unsigned i = 0; i < leads.size(); i++) {
    push_float_list(out, leads[i].getX());
    push_float_list(out, leads[i].getY());
    push_u32(out, 0);
    push_f32(out, static_cast<float>(leads[i].getProb()));
  }

  push_float_list(out, lane_stds);
  push_float_list(out, edge_stds);
  return out;
}
static std::vector<uint8_t> encode_live_calibration_typed(cereal::LiveCalibrationData::Reader lc) {
  std::vector<uint8_t> out;
  auto rpy = lc.getRpyCalib();
  auto h = lc.getHeight();
  out.reserve(64);
  push_u8(out, 0x01);  // schema version
  push_u32(out, static_cast<uint32_t>(rpy.size()));
  for (auto v : rpy) push_f32(out, static_cast<float>(v));
  push_u32(out, static_cast<uint32_t>(h.size()));
  for (auto v : h) push_f32(out, static_cast<float>(v));
  push_u32(out, static_cast<uint32_t>(lc.getCalStatus()));
  push_u32(out, static_cast<uint32_t>(lc.getCalPerc()));
  return out;
}
static std::vector<uint8_t> encode_car_output_typed(cereal::CarOutput::Reader co) {
  std::vector<uint8_t> out;
  out.reserve(1 + 4 * 9 + 4);
  const auto act = co.getActuatorsOutput();
  push_u8(out, 0x01);  // schema version
  push_f32(out, static_cast<float>(act.getTorque()));
  push_f32(out, static_cast<float>(act.getSteeringAngleDeg()));
  push_f32(out, static_cast<float>(act.getCurvature()));
  push_f32(out, static_cast<float>(act.getAccel()));
  push_u32(out, static_cast<uint32_t>(act.getLongControlState()));
  push_f32(out, static_cast<float>(act.getGas()));
  push_f32(out, static_cast<float>(act.getBrake()));
  push_f32(out, static_cast<float>(act.getTorqueOutputCan()));
  push_f32(out, static_cast<float>(act.getSpeed()));
  return out;
}

static std::vector<uint8_t> encode_live_parameters_typed(cereal::LiveParametersData::Reader lp) {
  std::vector<uint8_t> out;
  auto values = lp.getDebugFilterState().getValue();
  auto stds = lp.getDebugFilterState().getStd();
  out.reserve(160 + values.size() * 4 + stds.size() * 4);
  push_u8(out, 0x01);  // schema version
  push_u8(out, lp.getValid() ? 1 : 0);
  push_u8(out, lp.getSensorValid() ? 1 : 0);
  push_u8(out, lp.getPosenetValid() ? 1 : 0);
  push_f32(out, static_cast<float>(lp.getGyroBias()));
  push_f32(out, static_cast<float>(lp.getAngleOffsetDeg()));
  push_f32(out, static_cast<float>(lp.getAngleOffsetAverageDeg()));
  push_f32(out, static_cast<float>(lp.getStiffnessFactor()));
  push_f32(out, static_cast<float>(lp.getSteerRatio()));
  push_f32(out, static_cast<float>(lp.getRoll()));
  push_f32(out, static_cast<float>(lp.getPosenetSpeed()));
  push_f32(out, static_cast<float>(lp.getAngleOffsetFastStd()));
  push_f32(out, static_cast<float>(lp.getAngleOffsetAverageStd()));
  push_f32(out, static_cast<float>(lp.getStiffnessFactorStd()));
  push_f32(out, static_cast<float>(lp.getSteerRatioStd()));
  push_u8(out, lp.getAngleOffsetValid() ? 1 : 0);
  push_u8(out, lp.getAngleOffsetAverageValid() ? 1 : 0);
  push_u8(out, lp.getSteerRatioValid() ? 1 : 0);
  push_u8(out, lp.getStiffnessFactorValid() ? 1 : 0);

  push_u32(out, static_cast<uint32_t>(values.size()));
  for (auto v : values) push_f32(out, static_cast<float>(v));
  push_u32(out, static_cast<uint32_t>(stds.size()));
  for (auto v : stds) push_f32(out, static_cast<float>(v));
  return out;
}



static void encode_driver_pose_typed(std::vector<uint8_t>& out, cereal::DriverStateV2::DriverData::Reader d) {
  push_float_list(out, d.getFaceOrientation());
  push_float_list(out, d.getFaceOrientationStd());
  push_float_list(out, d.getFacePosition());
  push_float_list(out, d.getFacePositionStd());
  push_f32(out, static_cast<float>(d.getFaceProb()));
  push_f32(out, static_cast<float>(d.getLeftEyeProb()));
  push_f32(out, static_cast<float>(d.getRightEyeProb()));
  push_f32(out, static_cast<float>(d.getLeftBlinkProb()));
  push_f32(out, static_cast<float>(d.getRightBlinkProb()));
  push_f32(out, static_cast<float>(d.getSunglassesProb()));
  push_f32(out, static_cast<float>(d.getPhoneProb()));
}

static std::vector<uint8_t> encode_driver_state_v2_typed(cereal::DriverStateV2::Reader d) {
  std::vector<uint8_t> out;
  out.reserve(256);
  push_u8(out, 0x01);  // schema version
  push_u32(out, static_cast<uint32_t>(d.getFrameId()));
  push_f32(out, static_cast<float>(d.getModelExecutionTime()));
  push_f32(out, static_cast<float>(d.getGpuExecutionTime()));
  push_f32(out, static_cast<float>(d.getWheelOnRightProb()));
  encode_driver_pose_typed(out, d.getLeftDriverData());
  encode_driver_pose_typed(out, d.getRightDriverData());
  return out;
}
static std::vector<uint8_t> encode_onroad_events_typed(capnp::List<cereal::OnroadEvent>::Reader events) {
  std::vector<uint8_t> out;
  out.reserve(8 + events.size() * 16);
  push_u8(out, 0x01);  // schema version
  push_u32(out, static_cast<uint32_t>(events.size()));
  for (auto e : events) {
    push_u32(out, static_cast<uint32_t>(e.getName()));
    push_u8(out, e.getEnable() ? 1 : 0);
    push_u8(out, e.getNoEntry() ? 1 : 0);
    push_u8(out, e.getWarning() ? 1 : 0);
    push_u8(out, e.getUserDisable() ? 1 : 0);
    push_u8(out, e.getSoftDisable() ? 1 : 0);
    push_u8(out, e.getImmediateDisable() ? 1 : 0);
    push_u8(out, e.getPreEnable() ? 1 : 0);
    push_u8(out, e.getPermanent() ? 1 : 0);
    push_u8(out, e.getOverrideLateral() ? 1 : 0);
    push_u8(out, e.getOverrideLongitudinal() ? 1 : 0);
  }
  return out;
}
static std::vector<uint8_t> encode_driver_monitoring_typed(cereal::DriverMonitoringState::Reader dm) {
  std::vector<uint8_t> out;
  out.reserve(96);
  push_u8(out, 0x01);  // schema version
  push_u8(out, dm.getFaceDetected() ? 1 : 0);
  push_u8(out, dm.getIsDistracted() ? 1 : 0);
  push_u32(out, static_cast<uint32_t>(dm.getDistractedType()));
  push_f32(out, static_cast<float>(dm.getAwarenessStatus()));
  push_f32(out, static_cast<float>(dm.getAwarenessActive()));
  push_f32(out, static_cast<float>(dm.getAwarenessPassive()));
  push_f32(out, static_cast<float>(dm.getStepChange()));
  push_f32(out, static_cast<float>(dm.getPosePitchOffset()));
  push_u32(out, static_cast<uint32_t>(dm.getPosePitchValidCount()));
  push_f32(out, static_cast<float>(dm.getPoseYawOffset()));
  push_u32(out, static_cast<uint32_t>(dm.getPoseYawValidCount()));
  push_u8(out, dm.getIsLowStd() ? 1 : 0);
  push_u32(out, static_cast<uint32_t>(dm.getHiStdCount()));
  push_u8(out, dm.getIsActiveMode() ? 1 : 0);
  push_u8(out, dm.getIsRHD() ? 1 : 0);
  push_u32(out, static_cast<uint32_t>(dm.getUncertainCount()));
  return out;
}

static std::vector<uint8_t> encode_road_camera_state_typed(cereal::FrameData::Reader f) {
  std::vector<uint8_t> out;
  auto temps = f.getTemperaturesC();
  out.reserve(96 + temps.size() * 4);
  push_u8(out, 0x01);  // schema version
  push_u32(out, static_cast<uint32_t>(f.getFrameId()));
  push_u32(out, static_cast<uint32_t>(f.getFrameIdSensor()));
  push_u32(out, static_cast<uint32_t>(f.getRequestId()));
  push_u32(out, static_cast<uint32_t>(f.getEncodeId()));
  push_u64(out, static_cast<uint64_t>(f.getTimestampEof()));
  push_u64(out, static_cast<uint64_t>(f.getTimestampSof()));
  push_f32(out, static_cast<float>(f.getProcessingTime()));
  push_u32(out, static_cast<uint32_t>(f.getIntegLines()));
  push_f32(out, static_cast<float>(f.getGain()));
  push_u8(out, f.getHighConversionGain() ? 1 : 0);
  push_f32(out, static_cast<float>(f.getMeasuredGreyFraction()));
  push_f32(out, static_cast<float>(f.getTargetGreyFraction()));
  push_f32(out, static_cast<float>(f.getExposureValPercent()));
  push_u32(out, static_cast<uint32_t>(temps.size()));
  for (auto t : temps) push_f32(out, static_cast<float>(t));
  push_u32(out, static_cast<uint32_t>(f.getSensor()));
  return out;
}
static bool send_meta_raw_frame(int fd,
                                uint8_t service_index,
                                uint16_t event_which,
                                uint64_t log_mono_time,
                                const std::vector<uint8_t>& typed_payload,
                                const std::string& json_payload,
                                const uint8_t* raw_data,
                                size_t raw_size) {
  if (raw_data == nullptr || raw_size == 0) return true;
  const uint32_t typed_len = static_cast<uint32_t>(typed_payload.size());
  const uint32_t json_len = static_cast<uint32_t>(json_payload.size());
  const uint32_t raw_len = static_cast<uint32_t>(raw_size);
  std::vector<uint8_t> payload(1 + 1 + 2 + 8 + 4 + typed_len + 4 + json_len + 4 + raw_len);
  payload[0] = 0x03;  // raw meta envelope v3 includes typed + json snapshot + raw event
  payload[1] = service_index;
  put_be16(&payload[2], event_which);
  put_be64(&payload[4], log_mono_time);

  size_t off = 12;
  put_be32(&payload[off], typed_len);
  off += 4;
  if (typed_len > 0) {
    memcpy(&payload[off], typed_payload.data(), typed_len);
    off += typed_len;
  }

  put_be32(&payload[off], json_len);
  off += 4;
  if (json_len > 0) {
    memcpy(&payload[off], json_payload.data(), json_len);
    off += json_len;
  }

  put_be32(&payload[off], raw_len);
  off += 4;
  memcpy(&payload[off], raw_data, raw_len);
  return commaview::net::send_meta_bytes(fd, payload.data(), payload.size(), MSG_META_RAW);
}

static std::string stats_path_for_service(const char* service_name) {
  std::string safe = service_name ? service_name : "unknown";
  for (char& ch : safe) {
    if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_') ch = '_';
  }
  return std::string("/tmp/commaview-telemetry-") + safe + ".json";
}

static void write_telemetry_stats_file(const std::string& path,
                                       const char* service_name,
                                       uint64_t telem_json,
                                       uint64_t telem_raw,
                                       uint64_t telem_drop,
                                       uint64_t telem_drain,
                                       int emit_ms) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) return;
  out << "{"
      << "\"service\":\"" << (service_name ? service_name : "unknown") << "\","
      << "\"telemJson\":" << telem_json << ","
      << "\"telemRaw\":" << telem_raw << ","
      << "\"telemDrop\":" << telem_drop << ","
      << "\"telemDrain\":" << telem_drain << ","
      << "\"emitMs\":" << emit_ms
      << "}";
}

static void set_session_policy(const std::string& session_id, bool suppress_video) {
  commaview::control::set_session_policy(session_id, suppress_video);
}

static bool get_session_policy(const std::string& session_id, bool* suppress_video) {
  return commaview::control::get_session_policy(session_id, suppress_video);
}

using ClientControlState = commaview::control::ClientControlState;

static void consume_client_control_frames(int client_fd,
                                          ClientControlState* state,
                                          const char* video_service) {
  commaview::control::consume_client_control_frames(client_fd, state, video_service, MSG_CONTROL);
}

static bool send_all(int fd, const void* data, size_t len) {
  return commaview::net::send_all(fd, data, len);
}

static bool send_frame(int fd, const uint8_t* payload, size_t payload_len) {
  return commaview::net::send_frame(fd, payload, payload_len);
}

static bool send_meta_json(int fd, const std::string& json) {
  return commaview::net::send_meta_json(fd, json, MSG_META);
}

static bool client_socket_alive(int fd) {
  return commaview::net::client_socket_alive(fd);
}

static int create_server(int port) {
  return commaview::net::create_server(port);
}

static cereal::EncodeData::Reader read_encode_data(cereal::Event::Reader event, int port) {
  return commaview::video::read_encode_data(event, port, g_dev_mode);
}

static std::string build_telemetry_json(cereal::Event::Reader event) {
  return commaview::telemetry::build_telemetry_json(event);
}

static void handle_client(int client_fd, const char* video_service, int port) {
  char addr_str[64] = {};
  struct sockaddr_in peer = {};
  socklen_t peer_len = sizeof(peer);
  if (getpeername(client_fd, reinterpret_cast<struct sockaddr*>(&peer), &peer_len) == 0) {
    inet_ntop(AF_INET, &peer.sin_addr, addr_str, sizeof(addr_str));
  }

  printf("[%s] client connected: %s (fd=%d)\n", video_service, addr_str, client_fd);
  fflush(stdout);

  int opt = 1;
  setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  int sndbuf = 524288;
  setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  auto& active_counter = active_counter_for_port(port);
  int previous = active_counter.fetch_add(1);
  if (previous > 0) {
    // Keep one active client per stream to avoid msgq reader churn/races.
    active_counter.fetch_sub(1);
    close(client_fd);
    return;
  }

  Context* ctx = Context::create();

  const bool enable_video = !g_telemetry_only;
  SubSocket* video_sock = nullptr;
  if (enable_video) {
    const size_t video_segment_size = queue_size_for_service(video_service);
    video_sock = SubSocket::create(ctx, video_service, "127.0.0.1", true, true, video_segment_size);
  }

  const bool include_telemetry = !g_video_only && (port == PORT_ROAD || port == PORT_WIDE);
  SubSocket* telem_socks[NUM_TELEM] = {};

  if (include_telemetry) {
    for (int i = 0; i < NUM_TELEM; i++) {
      if (!telemetry_enabled_index(i)) continue;
      const size_t segment_size = queue_size_for_service(TELEMETRY_SERVICES[i]);
      telem_socks[i] = SubSocket::create(ctx, TELEMETRY_SERVICES[i], "127.0.0.1", true, true, segment_size);
    }
  }

  Poller* poller = Poller::create();
  if (video_sock != nullptr) {
    poller->registerSocket(video_sock);
  }
  if (include_telemetry && !g_telemetry_subscribe_only) {
    for (int i = 0; i < NUM_TELEM; i++) {
      if (telem_socks[i] != nullptr) poller->registerSocket(telem_socks[i]);
    }
  }

  uint64_t frame_count = 0;
  uint64_t parse_error_count = 0;
  uint64_t wrong_union_count = 0;
  uint64_t telem_count = 0;
  uint64_t telem_raw_count = 0;
  uint64_t telem_drop_count = 0;
  uint64_t telem_drain_count = 0;
  std::vector<commaview::telemetry::ServiceCounters> service_stats(NUM_TELEM);
  commaview::telemetry::LoopCounters loop_stats;
  uint64_t suppressed_video_count = 0;
  auto t0 = std::chrono::steady_clock::now();
  auto next_telem_emit = t0 + std::chrono::milliseconds(g_telemetry_emit_ms);
  auto next_stats_flush = t0 + std::chrono::milliseconds(1000);
  const std::string telemetry_stats_path = stats_path_for_service(video_service);
  std::string car_json_latest;
  std::string controls_json_latest;
  std::string device_json_latest;
  std::string model_json_latest;
  std::string radar_json_latest;
  std::string calibration_json_latest;
  std::string car_control_json_latest;
  std::string car_output_json_latest;
  std::string live_parameters_json_latest;
  std::string driver_monitoring_json_latest;
  std::string driver_state_v2_json_latest;
  std::string onroad_events_json_latest;
  std::string road_camera_state_json_latest;
  bool have_car_json = false;
  bool have_controls_json = false;
  bool have_device_json = false;
  bool have_model_json = false;
  bool have_radar_json = false;
  bool have_calibration_json = false;
  bool have_car_control_json = false;
  bool have_car_output_json = false;
  bool have_live_parameters_json = false;
  bool have_driver_monitoring_json = false;
  bool have_driver_state_v2_json = false;
  bool have_onroad_events_json = false;
  bool have_road_camera_state_json = false;
  std::vector<std::vector<uint8_t>> raw_latest(NUM_TELEM);
  std::vector<uint64_t> raw_log_mono(NUM_TELEM, 0);
  std::vector<uint16_t> raw_event_which(NUM_TELEM, 0);
  std::vector<std::string> raw_json_latest(NUM_TELEM);
  std::vector<std::vector<uint8_t>> raw_typed_latest(NUM_TELEM);
  std::vector<bool> have_raw(NUM_TELEM, false);
  AlignedBuffer aligned_buf;
  ClientControlState control_state;

  while (g_running) {
    const auto loop_started = std::chrono::steady_clock::now();
    if (!client_socket_alive(client_fd)) break;

    consume_client_control_frames(client_fd, &control_state, video_service);

    auto ready = poller->poll(20);

    for (auto* sock : ready) {
      int telem_sock_idx = -1;
      if (include_telemetry) {
        for (int i = 0; i < NUM_TELEM; i++) {
          if (sock == telem_socks[i]) {
            telem_sock_idx = i;
            break;
          }
        }
      }

      std::unique_ptr<Message> raw_msg(sock->receive(true));
      if (!raw_msg) continue;
      if (telem_sock_idx >= 0) {
        commaview::telemetry::note_ingest(service_stats, telem_sock_idx);
      }

      // Latest-only semantics for telemetry sockets: drain queued historical samples
      // and keep only the freshest message before decode/emit.
      if (telem_sock_idx >= 0 && !g_telemetry_subscribe_only) {
        while (true) {
          std::unique_ptr<Message> newer(sock->receive(true));
          if (!newer) break;
          raw_msg = std::move(newer);
          telem_drain_count++;
          commaview::telemetry::note_ingest(service_stats, telem_sock_idx);
          commaview::telemetry::note_coalesced(service_stats, telem_sock_idx);
        }
      }

      const size_t raw_size = raw_msg->getSize();

      if (include_telemetry && g_telemetry_drain_only && telem_sock_idx >= 0) {
        telem_drain_count++;
        commaview::telemetry::note_drop(service_stats, telem_sock_idx);
        if (telem_drain_count <= 3 || (telem_drain_count % 200) == 0) {
          printf("[%s] telem-drain=%llu raw=%zu [DRAIN_ONLY]\n",
                 video_service,
                 static_cast<unsigned long long>(telem_drain_count),
                 raw_size);
          fflush(stdout);
        }
        continue;
      }

      try {
        capnp::ReaderOptions options;
        options.traversalLimitInWords = kj::maxValue;

        capnp::FlatArrayMessageReader reader(aligned_buf.align(raw_msg.get()), options);
        auto event = reader.getRoot<cereal::Event>();

        if (video_sock != nullptr && sock == video_sock) {
          const auto which = event.which();
          const auto expected = expected_video_which_for_port(port);
          if (which != expected) {
            wrong_union_count++;
            if (wrong_union_count <= 20 || (wrong_union_count % 100) == 0) {
              printf("[%s] union mismatch #%llu: got=%d expected=%d raw=%zu\n",
                     video_service,
                     static_cast<unsigned long long>(wrong_union_count),
                     static_cast<int>(which),
                     static_cast<int>(expected),
                     raw_size);
              fflush(stdout);
            }
            continue;
          }

          auto ed = read_encode_data(event, port);
          auto header = ed.getHeader();
          auto data = ed.getData();

          const uint32_t header_len = header.size();
          const size_t data_len = data.size();
          if (data_len == 0) continue;

          bool suppress_video = g_suppress_video;
          bool session_policy = false;
          if (!suppress_video && get_session_policy(control_state.bound_session_id, &session_policy)) {
            suppress_video = session_policy;
          }

          if (suppress_video) {
            suppressed_video_count++;
            if (suppressed_video_count <= 3 || (suppressed_video_count % 500) == 0) {
              printf("[%s] suppress-video drop=%llu session=%s header=%u data=%zu\n",
                     video_service,
                     static_cast<unsigned long long>(suppressed_video_count),
                     control_state.bound_session_id.empty() ? "<legacy>" : control_state.bound_session_id.c_str(),
                     header_len,
                     data_len);
              fflush(stdout);
            }
            continue;
          }

          const size_t payload_len = 1 + 4 + header_len + data_len;
          std::vector<uint8_t> payload(payload_len);
          payload[0] = MSG_VIDEO;
          put_be32(&payload[1], header_len);
          if (header_len > 0) memcpy(&payload[5], header.begin(), header_len);
          memcpy(&payload[5 + header_len], data.begin(), data_len);

          if (!send_frame(client_fd, payload.data(), payload.size())) {
            goto disconnect;
          }

          frame_count++;
          if (frame_count <= 5 || (frame_count % 200) == 0) {
            auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - t0).count();
            const double fps = elapsed > 0 ? frame_count / elapsed : 0.0;
            printf("[%s] frames=%llu fps=%.1f header=%u data=%zu\n",
                   video_service,
                   static_cast<unsigned long long>(frame_count),
                   fps,
                   header_len,
                   data_len);
            fflush(stdout);
          }
          continue;
        }

        if (include_telemetry) {
          const auto which = event.which();
          const int raw_idx = telemetry_index_for_which(which);
          if (raw_idx >= 0) {
            const uint8_t* raw_ptr = reinterpret_cast<const uint8_t*>(raw_msg->getData());
            raw_latest[raw_idx].assign(raw_ptr, raw_ptr + raw_size);
            raw_log_mono[raw_idx] = event.getLogMonoTime();
            raw_event_which[raw_idx] = static_cast<uint16_t>(which);
            have_raw[raw_idx] = true;
          }
        }
      } catch (const std::exception& e) {
        parse_error_count++;
        printf("[%s] parse exception #%llu: %s (raw=%zu)\n",
               video_service,
               static_cast<unsigned long long>(parse_error_count),
               e.what(),
               raw_size);
        fflush(stdout);
      } catch (...) {
        parse_error_count++;
        printf("[%s] parse unknown exception #%llu (raw=%zu)\n",
               video_service,
               static_cast<unsigned long long>(parse_error_count),
               raw_size);
        fflush(stdout);
      }
    }

    if (include_telemetry) {
      auto now = std::chrono::steady_clock::now();
      if (now >= next_telem_emit) {
        next_telem_emit = now + std::chrono::milliseconds(g_telemetry_emit_ms);

        for (int i = 0; i < NUM_TELEM; i++) {
          if (!have_raw[i]) continue;
          const auto& raw = raw_latest[i];
          if (!send_meta_raw_frame(client_fd,
                                   static_cast<uint8_t>(i),
                                   raw_event_which[i],
                                   raw_log_mono[i],
                                   std::vector<uint8_t>{},
                                   std::string(),
                                   raw.data(),
                                   raw.size())) goto disconnect;
          telem_raw_count++;
          commaview::telemetry::note_emit_raw(service_stats, i);
        }

        const uint64_t telem_log_counter = telem_raw_count;
        if (telem_log_counter <= 3 || (telem_log_counter % 200) == 0) {
          printf("[%s] telem_raw=%llu drain=%llu (coalesced %dms)\n",
                 video_service,
                 static_cast<unsigned long long>(telem_raw_count),
                 static_cast<unsigned long long>(telem_drain_count),
                 g_telemetry_emit_ms);
          fflush(stdout);
        }

        if (now >= next_stats_flush) {
          next_stats_flush = now + std::chrono::milliseconds(1000);
          write_telemetry_stats_file(telemetry_stats_path,
                                     video_service,
                                     telem_count,
                                     telem_raw_count,
                                     telem_drop_count,
                                     telem_drain_count,
                                     g_telemetry_emit_ms);
          if (!commaview::telemetry::service_counter_invariants_hold(service_stats)) {
            printf("[%s] telemetry counter invariant violated: coalesced>ingest\n", video_service);
            fflush(stdout);
          }
        }
      }
    }

    const auto loop_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - loop_started);
    commaview::telemetry::note_loop(loop_stats,
                                    static_cast<uint64_t>(loop_elapsed.count()),
                                    g_telemetry_emit_ms);
  }

disconnect:
  printf("[%s] client disconnected: %s\n", video_service, addr_str);
  fflush(stdout);

  active_counter.fetch_sub(1);
  close(client_fd);

  delete poller;
  if (video_sock != nullptr) delete video_sock;
  for (int i = 0; i < NUM_TELEM; i++) {
    if (telem_socks[i] != nullptr) delete telem_socks[i];
  }
  delete ctx;
}

static void accept_loop(int server_fd, const char* service_name, int port) {
  printf("[%s] listening on :%d\n", service_name, port);
  fflush(stdout);

  while (g_running) {
    struct sockaddr_in client_addr = {};
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(server_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
    if (client_fd < 0) {
      if (g_running) perror("accept");
      continue;
    }
    std::thread(handle_client, client_fd, service_name, port).detach();
  }
}

static void sig_handler(int) { g_running = false; }

int commaview_bridge_main(int argc, char* argv[]) {
  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);
  signal(SIGPIPE, SIG_IGN);

  if (const char* env_emit = std::getenv("COMMAVIEW_TELEMETRY_EMIT_MS")) {
    g_telemetry_emit_ms = clamp_telem_emit_ms(parse_int_or_default(env_emit, g_telemetry_emit_ms));
  }


  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--dev") == 0) g_dev_mode = true;
    if (strcmp(argv[i], "--telem-emit-ms") == 0 && (i + 1) < argc) {
      g_telemetry_emit_ms = clamp_telem_emit_ms(parse_int_or_default(argv[++i], g_telemetry_emit_ms));
    }
  }

  const char** video_services = g_dev_mode ? VIDEO_SERVICES_DEV : VIDEO_SERVICES_PROD;
  printf("CommaView Bridge v3.3.8-safe-bundle (C++)%s [VIDEO+TELEMETRY][RAW_ONLY_DEFAULT][META_MODE=raw-only][EMIT_MS=%d]\n",
         g_dev_mode ? " [DEV MODE: livestream]" : "",
         g_telemetry_emit_ms);
  fflush(stdout);

  std::vector<std::pair<int, const char*>> streams;
  if (g_dev_mode) {
    streams.push_back({PORT_ROAD, video_services[0]});
  } else {
    streams.push_back({PORT_ROAD, video_services[0]});
    streams.push_back({PORT_WIDE, video_services[1]});
    streams.push_back({PORT_DRIVER, video_services[2]});
  }

  std::vector<std::thread> threads;
  std::vector<int> server_fds;

  for (auto& s : streams) {
    int fd = create_server(s.first);
    if (fd < 0) {
      fprintf(stderr, "failed on port %d\n", s.first);
      return 1;
    }
    server_fds.push_back(fd);
    threads.emplace_back(accept_loop, fd, s.second, s.first);
  }

  printf("ready. waiting for connections...\n");
  fflush(stdout);

  for (auto& t : threads) t.join();
  for (int fd : server_fds) close(fd);

  printf("CommaView Bridge stopped.\n");
  return 0;
}
#ifndef COMMAVIEW_BRIDGE_NO_MAIN
int main(int argc, char* argv[]) {
  return commaview_bridge_main(argc, argv);
}
#endif
