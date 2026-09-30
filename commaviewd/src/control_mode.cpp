#include "control_mode.h"
#include "gps_peek.h"
#include "http_server.h"
#include "runtime_debug_config.h"
#include "source_recording_archive.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <csignal>
#include <iomanip>
#include <cmath>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <mutex>
#include <ctime>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

extern char** environ;

namespace commaview::runtime {
namespace {

// Same helpers the runtime-debug renderer uses (identical implementations).
using commaview::runtime_debug::json_escape;
using commaview::runtime_debug::trim_copy;

constexpr const char* kInstallDir = "/data/commaview";
constexpr const char* kParamsDir = "/data/params/d";
constexpr int kDefaultApiPort = 5002;
constexpr int kDiscoveryPort = 5004;
constexpr const char* kDiscoveryQuery = "COMMAVIEW_DISCOVER_V1";
constexpr const char* kVersionFile = "/data/commaview/VERSION";
constexpr const char* kPairingScheme = "commaview://pair";
constexpr int kPairingCodeTtlSec = 300;
constexpr const char* kOnroadUiExportStatusFile = "/data/commaview/run/onroad-ui-export-status.json";
constexpr const char* kOnroadUiExportApplyScript = "/data/commaview/scripts/apply_onroad_ui_export_patch.sh";
constexpr const char* kOnroadUiExportVerifyScript = "/data/commaview/scripts/verify_onroad_ui_export_patch.sh";
constexpr size_t kSupportLogPerFileCapBytes = 512 * 1024;
constexpr size_t kSupportLogTotalCapBytes = 2 * 1024 * 1024;

struct SupportLogFileSpec {
  std::string entry_name;
  std::string path;
  bool rotated;
};

std::vector<SupportLogFileSpec> support_log_files() {
  // Put the small structured snapshots first. Large rolling logs can consume the
  // total response cap, but a support bundle must always retain the current
  // bounded counters and effective configuration needed to diagnose the run.
  std::vector<SupportLogFileSpec> files = {
      {"telemetry-stats.json", "/data/commaview/run/telemetry-stats.json", false},
      {"runtime-debug-effective.json", "/data/commaview/run/runtime-debug-effective.json", false},
      {"onroad-ui-export-status.json", "/data/commaview/run/onroad-ui-export-status.json", false},
      {"last-restart-reason.txt", "/data/commaview/run/last-restart-reason.txt", false},
      {"runtime-run-events.jsonl", "/data/commaview/logs/runtime-run-events.jsonl", false},
      {"commaviewd-bridge.log", "/data/commaview/logs/commaviewd-bridge.log", false},
      {"commaviewd-control.log", "/data/commaview/logs/commaviewd-control.log", false},
      {"onroad-ui-export-startup.log", "/data/commaview/logs/onroad-ui-export-startup.log", false},
      // The drive stats script's own log never holds positions.
      {"commaview-drive-stats.log", "/data/commaview/logs/commaview-drive-stats.log", false},
  };
  const std::array<std::string, 5> rotated = {{
      "commaviewd-bridge.log",
      "commaviewd-control.log",
      "onroad-ui-export-startup.log",
      "commaview-drive-stats.log",
      "runtime-run-events.jsonl",
  }};
  for (const auto& name : rotated) {
    for (int idx = 1; idx <= 14; ++idx) {
      const std::string rotated_name = name + "." + std::to_string(idx);
      const std::string rotated_path = "/data/commaview/logs/" + rotated_name;
      files.push_back({rotated_name, rotated_path, true});
    }
  }
  return files;
}

struct PairingGrant {
  std::string code;
  std::time_t expires_at = 0;
  bool used = true;
};

std::mutex g_pairing_mutex;
PairingGrant g_pairing_grant;
bool run_command(const std::vector<std::string>& args, int* exit_code, std::string* stdout_text, std::string* stderr_text);
bool is_onroad();

std::string normalize_code(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc)) out.push_back(static_cast<char>(std::toupper(uc)));
  }
  return out;
}

bool codes_equal(const std::string& a, const std::string& b) {
  return normalize_code(a) == normalize_code(b);
}

std::string random_pair_code() {
  static const char* alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  std::array<unsigned char, 8> bytes{};
  size_t filled = 0;
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    ssize_t n = read(fd, bytes.data(), bytes.size());
    close(fd);
    filled = n < 0 ? 0 : static_cast<size_t>(n);
  }
  if (filled < bytes.size()) {
    // /dev/urandom missing or short: fill the remaining bytes from rand().
    std::srand(static_cast<unsigned int>(std::time(nullptr) ^ getpid()));
    for (size_t i = filled; i < bytes.size(); ++i) {
      bytes[i] = static_cast<unsigned char>(std::rand() & 0xFF);
    }
  }

  std::string raw;
  raw.reserve(8);
  for (size_t i = 0; i < 8; ++i) raw.push_back(alphabet[bytes[i] % 32]);
  return raw.substr(0, 4) + "-" + raw.substr(4, 4);
}


bool file_executable(const char* path) {
  return path != nullptr && access(path, X_OK) == 0;
}

std::string read_file_raw(const std::string& path) {
  std::ifstream f(path);
  if (!f) return "";
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string read_file_trimmed(const std::string& path) {
  return trim_copy(read_file_raw(path));
}

std::string runtime_version() {
  const std::string v = read_file_trimmed(kVersionFile);
  return v.empty() ? "unknown" : v;
}

std::string telemetry_mode() {
  return "direct-v2-ui-export";
}

std::string device_dongle_id() {
  const std::string dongle_id = read_file_trimmed(std::string(kParamsDir) + "/DongleId");
  if (dongle_id.empty() || dongle_id == "UnregisteredDevice") return "";
  return dongle_id;
}

std::string device_hardware_serial() {
  return read_file_trimmed(std::string(kParamsDir) + "/HardwareSerial");
}

std::string device_model() {
  const std::string model = read_file_trimmed(std::string(kParamsDir) + "/HardwareModel");
  return model.empty() ? "comma" : model;
}

std::string discovery_device_name() {
  const std::string dongle_id = device_dongle_id();
  if (!dongle_id.empty()) return std::string("comma-") + dongle_id;
  const std::string serial = device_hardware_serial();
  if (!serial.empty()) return std::string("comma-") + serial;
  char hostname[128] = {0};
  if (gethostname(hostname, sizeof(hostname) - 1) == 0 && hostname[0] != '\0') {
    return hostname;
  }
  return "comma-device";
}

bool write_file(const std::string& path, const std::string& value, mode_t mode = 0644) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) return false;
  const ssize_t want = static_cast<ssize_t>(value.size());
  const ssize_t wrote = write(fd, value.data(), static_cast<size_t>(want));
  close(fd);
  return wrote == want;
}

void ensure_runtime_debug_dirs() {
  mkdir("/data/commaview/config", 0755);
  mkdir("/data/commaview/run", 0755);
}

uint64_t now_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string read_file_tail_capped(const std::string& path, size_t cap, bool* exists, bool* truncated) {
  if (exists) *exists = false;
  if (truncated) *truncated = false;

  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return "";
  if (exists) *exists = true;

  struct stat st{};
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return "";
  }

  off_t start = 0;
  if (static_cast<uint64_t>(st.st_size) > cap) {
    start = st.st_size - static_cast<off_t>(cap);
    if (truncated) *truncated = true;
  }
  if (lseek(fd, start, SEEK_SET) < 0) {
    close(fd);
    return "";
  }

  std::string out;
  out.resize(static_cast<size_t>(st.st_size - start));
  const ssize_t n = read(fd, out.data(), out.size());
  close(fd);
  if (n < 0) return "";
  out.resize(static_cast<size_t>(n));
  return out;
}

std::string support_logs_response_json() {
  std::ostringstream out;
  size_t total = 0;
  out << "{\"ok\":true,";
  out << "\"generatedAtMs\":" << now_ms() << ",";
  out << "\"perFileCapBytes\":" << kSupportLogPerFileCapBytes << ",";
  out << "\"totalCapBytes\":" << kSupportLogTotalCapBytes << ",";
  out << "\"files\":[";

  bool first = true;
  const std::vector<SupportLogFileSpec> log_files = support_log_files();
  for (const auto& spec : log_files) {
    bool exists = false;
    bool truncated = false;
    std::string body = read_file_tail_capped(spec.path, kSupportLogPerFileCapBytes, &exists, &truncated);
    if (total + body.size() > kSupportLogTotalCapBytes) {
      truncated = true;
      const size_t remaining = total >= kSupportLogTotalCapBytes ? 0 : kSupportLogTotalCapBytes - total;
      body.resize(std::min(body.size(), remaining));
    }
    total += body.size();

    if (!first) out << ",";
    first = false;
    out << "{";
    out << "\"name\":\"" << json_escape(spec.entry_name) << "\",";
    out << "\"path\":\"" << json_escape(spec.path) << "\",";
    out << "\"exists\":" << (exists ? "true" : "false") << ",";
    out << "\"truncated\":" << (truncated ? "true" : "false") << ",";
    out << "\"rotated\":" << (spec.rotated ? "true" : "false") << ",";
    out << "\"encoding\":\"text\",";
    out << "\"content\":\"" << json_escape(body) << "\"";
    out << "}";

    if (total >= kSupportLogTotalCapBytes) break;
  }

  out << "],\"totalBytes\":" << total << "}";
  return out.str();
}

std::string runtime_restart_reason() {
  const char* from_env = std::getenv("COMMAVIEWD_RESTART_REASON");
  if (from_env != nullptr) {
    const std::string value = trim_copy(from_env);
    if (!value.empty()) return value;
  }
  const std::string from_file = read_file_trimmed("/data/commaview/run/last-restart-reason.txt");
  return from_file.empty() ? "startup" : from_file;
}

commaview::runtime_debug::LoadedRuntimeDebugConfig load_persisted_runtime_debug_config() {
  return commaview::runtime_debug::load_runtime_debug_config();
}

commaview::runtime_debug::LoadedRuntimeDebugConfig load_effective_runtime_debug_config_state() {
  auto persisted = load_persisted_runtime_debug_config();
  auto effective = commaview::runtime_debug::effective_runtime_debug_config(persisted);
  const std::string effective_path = commaview::runtime_debug::runtime_debug_effective_path();
  const std::string raw = read_file_raw(effective_path);
  if (!trim_copy(raw).empty()) {
    auto parsed = commaview::runtime_debug::parse_runtime_debug_config_body(raw, true, effective_path);
    if (parsed.valid) {
      effective = parsed;
      effective.exists = true;
      effective.valid = true;
      effective.safe_fallback = persisted.safe_fallback;
      effective.warnings = persisted.warnings;
    }
  }
  return effective;
}

std::string default_runtime_stats_json(const commaview::runtime_debug::LoadedRuntimeDebugConfig& effective) {
  std::ostringstream out;
  out << "{";
  out << "\"uptimeMs\":0,";
  out << "\"reconnectCount\":0,";
  out << "\"configVersion\":" << effective.config_version << ",";
  out << "\"configHash\":\"" << json_escape(effective.config_hash) << "\",";
  out << "\"lastRestartReason\":\"" << json_escape(runtime_restart_reason()) << "\",";
  out << "\"telemetryLoop\":{\"iterations\":0,\"avgMicros\":0,\"maxMicros\":0,\"overBudget\":0},";
  out << "\"videoLoop\":{\"iterations\":0,\"avgMicros\":0,\"maxMicros\":0,\"overBudget\":0},";
  out << "\"services\":{}";
  out << "}";
  return out.str();
}

std::string load_runtime_stats_json(const commaview::runtime_debug::LoadedRuntimeDebugConfig& effective) {
  const std::string raw = read_file_trimmed(commaview::runtime_debug::runtime_debug_stats_path());
  return raw.empty() ? default_runtime_stats_json(effective) : raw;
}

std::string runtime_debug_state_json() {
  const auto persisted = load_persisted_runtime_debug_config();
  const auto effective = load_effective_runtime_debug_config_state();
  const std::vector<std::string> warnings = !effective.warnings.empty() ? effective.warnings : persisted.warnings;
  std::ostringstream out;
  out << "{";
  out << "\"persistedConfig\":" << commaview::runtime_debug::render_config_json(persisted, true) << ",";
  out << "\"effectiveConfig\":" << commaview::runtime_debug::render_config_json(effective, true) << ",";
  out << "\"runtimeStats\":" << load_runtime_stats_json(effective) << ",";
  out << "\"warnings\":" << commaview::runtime_debug::warnings_json(warnings) << ",";
  out << "\"safeFallback\":" << ((persisted.safe_fallback || effective.safe_fallback) ? "true" : "false");
  out << "}";
  return out.str();
}

bool json_field_true(const std::string& body, const char* key) {
  if (key == nullptr) return false;
  return body.find(std::string("\"") + key + "\":true") != std::string::npos;
}

std::string onroad_ui_export_status_error_json(const std::string& state, const std::string& reason) {
  return std::string("{\"healthy\":false,\"patchVerified\":false,\"statusScope\":\"patch-installation\",\"repairNeeded\":true,\"state\":\"") +
         json_escape(state.empty() ? "error" : state) + "\",\"reason\":\"" +
         json_escape(reason.empty() ? "onroad UI export verify failed" : reason) + "\"}";
}

std::string load_onroad_ui_export_status_json() {
  const std::string raw = read_file_trimmed(kOnroadUiExportStatusFile);
  return raw.empty() ? onroad_ui_export_status_error_json("missing", "onroad UI export status unavailable") : raw;
}

std::string run_onroad_ui_export_verify_json(int* rc_out, std::string* err_out) {
  if (rc_out) *rc_out = 0;
  if (err_out) err_out->clear();
  if (!file_executable(kOnroadUiExportVerifyScript)) {
    return load_onroad_ui_export_status_json();
  }

  int rc = 0;
  std::string out;
  std::string err;
  if (!run_command({kOnroadUiExportVerifyScript, "--json"}, &rc, &out, &err)) {
    return load_onroad_ui_export_status_json();
  }

  if (rc_out) *rc_out = rc;
  if (err_out) *err_out = trim_copy(err);

  const std::string body = trim_copy(out);
  if (!body.empty()) return body;
  if (rc == 0) return load_onroad_ui_export_status_json();
  return onroad_ui_export_status_error_json("error", trim_copy(err));
}

std::string run_onroad_ui_export_apply_status_json(int* rc_out, std::string* err_out, bool force_offroad = false) {
  if (rc_out) *rc_out = 1;
  if (err_out) err_out->clear();
  if (is_onroad() && !force_offroad) {
    if (err_out) *err_out = "repair blocked while onroad";
    return onroad_ui_export_status_error_json("onroad-blocked", "repair blocked while onroad");
  }
  if (!file_executable(kOnroadUiExportApplyScript)) {
    if (err_out) *err_out = "onroad UI export repair helper missing";
    return onroad_ui_export_status_error_json("missing-helper", "onroad UI export repair helper missing");
  }

  int rc = 0;
  std::string out;
  std::string err;
  std::vector<std::string> args{kOnroadUiExportApplyScript};
  if (force_offroad) args.push_back("--force-offroad");
  const bool ran = run_command(args, &rc, &out, &err);
  const std::string trimmed_err = trim_copy(err);
  const std::string failure = trimmed_err.empty() ? "onroad UI export repair failed" : trimmed_err;
  if (!ran) {
    if (err_out) *err_out = failure;
    return onroad_ui_export_status_error_json("error", failure);
  }

  if (rc_out) *rc_out = rc;
  if (err_out) *err_out = trimmed_err;

  const std::string body = trim_copy(out);
  if (!body.empty()) return body;
  if (rc == 0) return load_onroad_ui_export_status_json();
  return onroad_ui_export_status_error_json("error", failure);
}

std::string live_onroad_ui_export_status_json(bool allow_self_heal) {
  int verify_rc = 0;
  std::string verify_err;
  std::string status = run_onroad_ui_export_verify_json(&verify_rc, &verify_err);

  if (!allow_self_heal) return status;
  if (is_onroad()) return status;
  if (!file_executable(kOnroadUiExportApplyScript)) return status;
  if (verify_rc == 0 && !json_field_true(status, "repairNeeded")) return status;

  int repair_rc = 0;
  std::string repair_err;
  const std::string repaired = run_onroad_ui_export_apply_status_json(&repair_rc, &repair_err);
  if (repair_rc == 0 || json_field_true(repaired, "patchVerified")) return repaired;
  return status;
}

std::string discovery_response_json() {
  const std::string version = runtime_version();
  const std::string dongleId = device_dongle_id();
  const std::string hardwareSerial = device_hardware_serial();
  const std::string model = device_model();
  std::ostringstream out;
  out << "{";
  out << "\"type\":\"commaview.discovery.v1\",";
  out << "\"name\":\"" << json_escape(discovery_device_name()) << "\",";
  out << "\"version\":\"" << json_escape(version) << "\",";
  out << "\"runtimeVersion\":\"" << json_escape(version) << "\",";
  out << "\"dongleId\":\"" << json_escape(dongleId) << "\",";
  out << "\"dongle_id\":\"" << json_escape(dongleId) << "\",";
  out << "\"deviceModel\":\"" << json_escape(model) << "\",";
  out << "\"device\":\"" << json_escape(model) << "\",";
  out << "\"hardwareSerial\":\"" << json_escape(hardwareSerial) << "\",";
  out << "\"apiPort\":" << kDefaultApiPort << ",";
  out << "\"api_port\":" << kDefaultApiPort << ",";
  out << "\"videoPorts\":{\"road\":8200,\"wide\":8201,\"driver\":8202}";
  out << "}";
  return out.str();
}

void discovery_responder_loop() {
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    std::perror("commaviewd discovery socket");
    return;
  }

  int opt = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
  setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(kDiscoveryPort));
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::perror("commaviewd discovery bind");
    ::close(fd);
    return;
  }

  std::printf("commaviewd discovery: listening on udp :%d\n", kDiscoveryPort);
  std::fflush(stdout);

  std::array<char, 512> buf{};
  while (true) {
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    const ssize_t n = ::recvfrom(fd, buf.data(), buf.size() - 1, 0, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (n <= 0) continue;
    std::string query(buf.data(), static_cast<size_t>(n));
    query = trim_copy(query);
    if (query != kDiscoveryQuery) continue;

    const std::string response = discovery_response_json();
    ::sendto(fd, response.data(), response.size(), 0, reinterpret_cast<sockaddr*>(&peer), peer_len);
  }
}

void start_discovery_responder() {
  std::thread(discovery_responder_loop).detach();
}

std::string runtime_status_json() {
  const std::string version = runtime_version();
  const std::string telemetryMode = telemetry_mode();
  const std::string dongleId = device_dongle_id();
  const std::string hardwareSerial = device_hardware_serial();
  const bool onroad = is_onroad();
  const auto persisted = load_persisted_runtime_debug_config();
  const auto effective = load_effective_runtime_debug_config_state();
  const std::vector<std::string> warnings = !effective.warnings.empty() ? effective.warnings : persisted.warnings;
  std::ostringstream out;
  out << "{";
  out << "\"version\":\"" << json_escape(version) << "\",";
  out << "\"runtimeVersion\":\"" << json_escape(version) << "\",";
  out << "\"dongleId\":\"" << json_escape(dongleId) << "\",";
  out << "\"dongle_id\":\"" << json_escape(dongleId) << "\",";
  out << "\"hardwareSerial\":\"" << json_escape(hardwareSerial) << "\",";
  out << "\"api_port\":" << kDefaultApiPort << ",";
  out << "\"telemetryMode\":\"" << json_escape(telemetryMode) << "\",";
  out << "\"roadState\":\"" << (onroad ? "onroad" : "offroad") << "\",";
  out << "\"isOnroad\":" << (onroad ? "true" : "false") << ",";
  out << "\"onroad\":" << (onroad ? "true" : "false") << ",";
  out << "\"onroadUiExport\":" << live_onroad_ui_export_status_json(false) << ",";
  out << "\"persistedConfig\":" << commaview::runtime_debug::render_config_json(persisted, true) << ",";
  out << "\"effectiveConfig\":" << commaview::runtime_debug::render_config_json(effective, true) << ",";
  out << "\"runtimeStats\":" << load_runtime_stats_json(effective) << ",";
  out << "\"configVersion\":" << effective.config_version << ",";
  out << "\"configHash\":\"" << json_escape(effective.config_hash) << "\",";
  out << "\"warnings\":" << commaview::runtime_debug::warnings_json(warnings) << ",";
  out << "\"safeFallback\":" << ((persisted.safe_fallback || effective.safe_fallback) ? "true" : "false");
  out << "}";
  return out.str();
}

bool write_runtime_debug_config_json(const std::string& body,
                                     commaview::runtime_debug::LoadedRuntimeDebugConfig* parsed_out,
                                     std::string* error_out) {
  auto parsed = commaview::runtime_debug::parse_runtime_debug_config_body(
      body,
      true,
      commaview::runtime_debug::runtime_debug_config_path());
  if (!parsed.valid) {
    if (error_out != nullptr) {
      *error_out = parsed.error.empty() ? "invalid runtime debug config" : parsed.error;
    }
    return false;
  }
  ensure_runtime_debug_dirs();
  const std::string canonical = commaview::runtime_debug::canonical_config_contents_json(parsed);
  if (!write_file(commaview::runtime_debug::runtime_debug_config_path(), canonical, 0644)) {
    if (error_out != nullptr) *error_out = "failed to write runtime debug config";
    return false;
  }
  parsed.exists = true;
  parsed.valid = true;
  parsed.safe_fallback = false;
  parsed.warnings.clear();
  if (parsed_out != nullptr) *parsed_out = parsed;
  return true;
}

std::string runtime_debug_write_response(bool ok,
                                         const commaview::runtime_debug::LoadedRuntimeDebugConfig& persisted,
                                         const std::string& error = "") {
  auto effective = commaview::runtime_debug::effective_runtime_debug_config(persisted);
  std::ostringstream out;
  out << "{";
  out << "\"ok\":" << (ok ? "true" : "false") << ",";
  out << "\"persistedConfig\":" << commaview::runtime_debug::render_config_json(persisted, true) << ",";
  out << "\"effectiveConfig\":" << commaview::runtime_debug::render_config_json(effective, true);
  if (!error.empty()) out << ",\"error\":\"" << json_escape(error) << "\"";
  out << "}";
  return out.str();
}

std::string runtime_debug_restore_defaults_response() {
  commaview::runtime_debug::LoadedRuntimeDebugConfig parsed;
  std::string error;
  if (!write_runtime_debug_config_json(commaview::runtime_debug::default_runtime_debug_config_json(), &parsed, &error)) {
    return runtime_debug_write_response(false, load_persisted_runtime_debug_config(), error);
  }
  return runtime_debug_write_response(true, parsed);
}

std::string runtime_debug_apply_response() {
  const auto persisted = load_persisted_runtime_debug_config();
  if (!persisted.valid) {
    return runtime_debug_write_response(false, persisted, persisted.error.empty() ? "invalid runtime debug config" : persisted.error);
  }
  ensure_runtime_debug_dirs();
  const std::string restart_cmd =
      "(sleep 1; COMMAVIEWD_RESTART_REASON=runtime-debug-apply bash /data/commaview/start.sh >/data/commaview/logs/runtime-debug-apply.log 2>&1) </dev/null &";
  int restart_rc = -1;
  std::string restart_out;
  std::string restart_err;
  const bool launched = run_command({"/bin/sh", "-lc", restart_cmd}, &restart_rc, &restart_out, &restart_err);
  const bool restart_ok = launched && restart_rc == 0;
  auto effective = commaview::runtime_debug::effective_runtime_debug_config(persisted);
  std::ostringstream out;
  out << "{";
  out << "\"ok\":" << (restart_ok ? "true" : "false") << ",";
  out << "\"restartScheduled\":" << (restart_ok ? "true" : "false") << ",";
  out << "\"persistedConfig\":" << commaview::runtime_debug::render_config_json(persisted, true) << ",";
  out << "\"effectiveConfig\":" << commaview::runtime_debug::render_config_json(effective, true);
  if (!restart_ok) out << ",\"error\":\"failed to schedule restart\"";
  out << "}";
  return out.str();
}
std::string read_param(const char* key) {
  std::string root = kParamsDir;
#if !defined(__aarch64__)
  // Host integration tests need an isolated params tree; device builds never
  // permit overriding the vehicle road-state source.
  if (const char* test_root = std::getenv("COMMAVIEWD_TEST_PARAMS_DIR")) {
    if (*test_root) root = test_root;
  }
#endif
  return read_file_trimmed(root + "/" + key);
}

bool is_onroad() {
  // Sunnypilot publishes IsOffroad, not IsOnroad. Prefer the actual device
  // state so the status API and offroad-only mutations agree with the car.
  const std::string offroad = read_param("IsOffroad");
  if (offroad == "0") return true;
  if (offroad == "1") return false;
  const std::string onroad = read_param("IsOnroad");
  if (onroad == "1") return true;
  if (onroad == "0") return false;
  // An unknown state must not authorize offroad-only changes.
  return true;
}

// Raw text between the quotes after the first `"key":` - no escape decoding,
// and an empty value counts as missing (returns false, *value_out == "").
// Deliberately differs from commaview::runtime_debug::extract_string_field.
bool extract_raw_string_field(const std::string& body, const char* key, std::string* value_out) {
  if (key == nullptr || value_out == nullptr) return false;
  const std::string needle = std::string("\"") + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos) return false;
  pos = body.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  pos++;
  while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) pos++;
  if (pos >= body.size() || body[pos] != '"') return false;
  pos++;
  size_t end = body.find('"', pos);
  if (end == std::string::npos) return false;
  *value_out = body.substr(pos, end - pos);
  return !value_out->empty();
}

std::string load_api_token() {
  const char* direct = std::getenv("COMMAVIEWD_API_TOKEN");
  if (direct != nullptr) {
    std::string token = trim_copy(direct);
    if (!token.empty()) return token;
  }

  const char* token_file_env = std::getenv("COMMAVIEWD_API_TOKEN_FILE");
  std::string token_file = token_file_env ? token_file_env : std::string(kInstallDir) + "/api/auth.token";
  return read_file_trimmed(token_file);
}

bool run_command(const std::vector<std::string>& args,
                 int* exit_code,
                 std::string* stdout_text,
                 std::string* stderr_text) {
  if (args.empty()) return false;

  int out_pipe[2];
  int err_pipe[2];
  if (pipe(out_pipe) != 0) return false;
  if (pipe(err_pipe) != 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(out_pipe[0]); close(out_pipe[1]);
    close(err_pipe[0]); close(err_pipe[1]);
    return false;
  }

  if (pid == 0) {
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);

    close(out_pipe[0]); close(out_pipe[1]);
    close(err_pipe[0]); close(err_pipe[1]);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    execvp(argv[0], argv.data());
    _exit(127);
  }

  close(out_pipe[1]);
  close(err_pipe[1]);

  auto read_all = [](int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    while (true) {
      ssize_t n = read(fd, buf.data(), buf.size());
      if (n <= 0) break;
      out.append(buf.data(), static_cast<size_t>(n));
    }
    return out;
  };

  std::string out = read_all(out_pipe[0]);
  std::string err = read_all(err_pipe[0]);
  close(out_pipe[0]);
  close(err_pipe[0]);

  int status = 0;
  waitpid(pid, &status, 0);

  if (stdout_text) *stdout_text = out;
  if (stderr_text) *stderr_text = err;

  if (exit_code) {
    if (WIFEXITED(status)) *exit_code = WEXITSTATUS(status);
    else *exit_code = 128;
  }

  return true;
}

bool run_command_with_optional_sudo(const std::vector<std::string>& args,
                                    int* exit_code,
                                    std::string* stdout_text,
                                    std::string* stderr_text) {
  int local_rc = 0;
  int* rc = exit_code ? exit_code : &local_rc;

  if (run_command(args, rc, stdout_text, stderr_text) && *rc == 0) {
    return true;
  }
  if (geteuid() == 0) {
    return false;
  }

  std::vector<std::string> sudo_args = {"sudo", "-n"};
  sudo_args.insert(sudo_args.end(), args.begin(), args.end());
  if (!run_command(sudo_args, rc, stdout_text, stderr_text)) {
    return false;
  }
  return *rc == 0;
}

// Change only the active Wi-Fi connection. Never edit NetworkManager's global
// default or a saved profile that is not currently connected.
bool active_wifi_profile(std::string* profile, std::string* error_json) {
  int rc = 0;
  std::string ignored;
  if (!run_command({"nmcli", "-g", "GENERAL.CONNECTION", "device", "show", "wlan0"},
                   &rc, profile, &ignored) || rc != 0) {
    *error_json = "{\"ok\":false,\"error\":\"Wi-Fi connection unavailable\"}";
    return false;
  }
  *profile = trim_copy(*profile);
  if (profile->empty() || *profile == "--") {
    *error_json = "{\"ok\":false,\"error\":\"Wi-Fi not connected\"}";
    return false;
  }
  return true;
}

bool offroad_and_disengaged() {
  return read_param("IsOffroad") == "1" && read_param("IsEngaged") == "0";
}

std::string wifi_power_save_status_response() {
  std::string profile, error_json;
  if (!active_wifi_profile(&profile, &error_json)) return error_json;
  int rc = 0;
  std::string ignored, configured;
  if (!run_command({"nmcli", "-g", "802-11-wireless.powersave", "connection", "show", profile},
                   &rc, &configured, &ignored) || rc != 0) {
    return "{\"ok\":false,\"error\":\"Wi-Fi power-save state unavailable\"}";
  }
  configured = trim_copy(configured);
  if (configured != "enable" && configured != "disable" && configured != "default") {
    return "{\"ok\":false,\"error\":\"Unknown Wi-Fi power-save state\"}";
  }
  return std::string("{\"ok\":true,\"mode\":\"") + configured + "\",\"enabled\":" +
         (configured == "default" ? "null" : configured == "enable" ? "true" : "false") + "}";
}

std::string wifi_power_save_set_response(const std::string& body) {
  std::string mode;
  if (!extract_raw_string_field(body, "mode", &mode) || (mode != "on" && mode != "off")) {
    return "{\"ok\":false,\"error\":\"mode must be on or off\"}";
  }
  if (!offroad_and_disengaged()) {
    return "{\"ok\":false,\"error\":\"offroad and disengaged required\"}";
  }
  std::string profile, error_json;
  if (!active_wifi_profile(&profile, &error_json)) return error_json;
  // Re-check after the nmcli round trip, right before the write.
  if (!offroad_and_disengaged()) {
    return "{\"ok\":false,\"error\":\"offroad and disengaged required\"}";
  }
  int rc = 0;
  std::string ignored;
  const std::string value = mode == "on" ? "3" : "2";
  if (!run_command_with_optional_sudo({"nmcli", "connection", "modify", "id", profile,
                                      "802-11-wireless.powersave", value},
                                      &rc, &ignored, &ignored)) {
    return "{\"ok\":false,\"error\":\"Could not save Wi-Fi setting\"}";
  }
  // This hardware cannot reapply power saving to an active Wi-Fi link. Do not
  // bounce the network under the tablet: the saved choice takes effect after
  // the next normal reconnect.
  return std::string("{\"ok\":true,\"mode\":\"") + (mode == "on" ? "enable" : "disable") +
         "\",\"enabled\":" + (mode == "on" ? "true" : "false") + ",\"reconnectRequired\":true}";
}

// ---- Drive stats and location, from what the comma already keeps: its onroad flag, its drive
// logs, and the positions sunnypilot saves. Nothing here subscribes to openpilot's messages. A short
// script (commaview_drive_stats.py) runs after each drive to read the drive's logs and comma's
// totals; the API only reads files, and deletes the location files the moment location is turned off.

std::string drivelog_root() {
#if !defined(__aarch64__)
  // Host integration tests need an isolated tree; device builds always use the install dir.
  if (const char* test_root = std::getenv("COMMAVIEWD_TEST_DATA_ROOT")) {
    if (*test_root) return test_root;
  }
#endif
  return kInstallDir;
}

// The position openpilot's last finished log minute gives, kept in memory (tmpfs) only.
std::string drivelog_live_dir() {
#if !defined(__aarch64__)
  if (const char* test_dir = std::getenv("COMMAVIEWD_TEST_LIVE_DIR")) {
    if (*test_dir) return test_dir;
  }
#endif
  return "/dev/shm/commaview";
}

// sunnypilot keeps the current position in memory params while it drives (its map helper).
std::string memory_params_dir() {
#if !defined(__aarch64__)
  if (const char* test_dir = std::getenv("COMMAVIEWD_TEST_MEM_PARAMS_DIR")) {
    if (*test_dir) return test_dir;
  }
#endif
  return "/dev/shm/params/d";
}

// openpilot's msgq rings, read without subscribing (see gps_peek.h).
std::string msgq_dir() {
#if !defined(__aarch64__)
  if (const char* test_dir = std::getenv("COMMAVIEWD_TEST_MSGQ_DIR")) {
    if (*test_dir) return test_dir;
  }
#endif
  return "/dev/shm";
}

std::string drivelog_path(const std::string& relative) {
  return drivelog_root() + "/" + relative;
}

std::string params_path(const std::string& key) {
  std::string root = kParamsDir;
#if !defined(__aarch64__)
  if (const char* test_root = std::getenv("COMMAVIEWD_TEST_PARAMS_DIR")) {
    if (*test_root) root = test_root;
  }
#endif
  return root + "/" + key;
}

constexpr size_t kDrivelogFileCapBytes = 256 * 1024;
constexpr int64_t kValidWallMs = 1704067200000LL;  // 2024: before that the clock hasn't been set
constexpr int kLivePositionMaxAgeSec = 30;
constexpr int kTotalsStaleSec = 30 * 60;
constexpr int kTotalsRefreshSec = 6 * 60 * 60;
constexpr int kTotalsRequestGapSec = 5 * 60;
constexpr int kPositionRunGapSec = 60;
constexpr int kAfterDriveDelaySec = 30;  // lets loggerd close the drive's last log first
constexpr int kMinDriveSec = 10;
constexpr int kDriveScriptTimeoutSec = 10 * 60;

int64_t wall_ms_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t file_mtime_ms(const std::string& path) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) return 0;
  return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
}

// A file the drive stats script wrote, embedded as it is, or null when it's missing, too big or not an object.
std::string embedded_json_object(const std::string& path) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      static_cast<size_t>(st.st_size) > kDrivelogFileCapBytes) {
    return "null";
  }
  const std::string raw = read_file_trimmed(path);
  if (raw.size() < 2 || raw.front() != '{' || raw.back() != '}') return "null";
  return raw;
}

// The value after the first `"key":`, spaces allowed: a JSON boolean or a number.
size_t json_value_start(const std::string& body, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos) return std::string::npos;
  pos = body.find_first_not_of(" \t\r\n", pos + needle.size());
  if (pos == std::string::npos || body[pos] != ':') return std::string::npos;
  return body.find_first_not_of(" \t\r\n", pos + 1);
}

bool extract_bool_field(const std::string& body, const char* key, bool* value_out) {
  if (key == nullptr || value_out == nullptr) return false;
  const size_t pos = json_value_start(body, key);
  if (pos == std::string::npos) return false;
  if (body.compare(pos, 4, "true") == 0) {
    *value_out = true;
    return true;
  }
  if (body.compare(pos, 5, "false") == 0) {
    *value_out = false;
    return true;
  }
  return false;
}

bool extract_number_field(const std::string& body, const char* key, double* value_out) {
  if (key == nullptr || value_out == nullptr) return false;
  const size_t pos = json_value_start(body, key);
  if (pos == std::string::npos) return false;
  const char* begin = body.c_str() + pos;
  char* end = nullptr;
  const double value = std::strtod(begin, &end);
  if (end == begin || !std::isfinite(value)) return false;
  *value_out = value;
  return true;
}

// Owner-only: these are where and how this comma drives.
bool write_file_atomic(const std::string& path, const std::string& value) {
  const std::string tmp = path + ".tmp";
  if (!write_file(tmp, value, 0600)) {
    unlink(tmp.c_str());
    return false;
  }
  return rename(tmp.c_str(), path.c_str()) == 0;
}

bool location_sharing_enabled() {
  bool enabled = false;
  return extract_bool_field(read_file_raw(drivelog_path("config/location.json")), "enabled", &enabled) && enabled;
}

// A position sunnypilot saved ({"latitude": .., "longitude": .., "bearing": ..}) as this API's
// position, dated by when it was saved; null when it isn't a real position.
std::string sunnypilot_position_json(const std::string& path, int max_age_sec) {
  const int64_t saved_ms = file_mtime_ms(path);
  if (saved_ms <= 0 || (max_age_sec > 0 && wall_ms_now() - saved_ms > static_cast<int64_t>(max_age_sec) * 1000)) {
    return "null";
  }
  const std::string raw = read_file_raw(path);
  double lat = 0.0, lon = 0.0, bearing = 0.0;
  if (!extract_number_field(raw, "latitude", &lat) || !extract_number_field(raw, "longitude", &lon) ||
      lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0 || (std::fabs(lat) < 1e-6 && std::fabs(lon) < 1e-6)) {
    return "null";
  }
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out << "{\"lat\":" << std::setprecision(6) << lat << ",\"lon\":" << lon;
  if (extract_number_field(raw, "bearing", &bearing)) out << ",\"bearingDeg\":" << std::setprecision(1) << bearing;
  out << ",\"fixMs\":" << saved_ms << ",\"source\":\"sunnypilot\"}";
  return out.str();
}

std::string comma_fix_json(const commaview::gps::Fix& fix, const std::string& extra = "") {
  std::ostringstream out;
  out.setf(std::ios::fixed);
  out << "{\"lat\":" << std::setprecision(6) << fix.lat << ",\"lon\":" << fix.lon
      << ",\"accuracyM\":" << std::setprecision(1) << fix.accuracy_m
      << ",\"bearingDeg\":" << fix.bearing_deg
      << ",\"speedMs\":" << std::setprecision(2) << fix.speed_ms
      << ",\"fixMs\":" << fix.fix_ms << extra << ",\"source\":\"comma\"}";
  return out.str();
}

enum class RoadState { kOnroad, kOffroad, kUnknown };

RoadState road_state() {
  const std::string offroad = read_param("IsOffroad");
  if (offroad == "0") return RoadState::kOnroad;
  if (offroad == "1") return RoadState::kOffroad;
  const std::string onroad = read_param("IsOnroad");
  if (onroad == "1") return RoadState::kOnroad;
  if (onroad == "0") return RoadState::kOffroad;
  return RoadState::kUnknown;
}

// ---- The drive watcher: notices each drive from the onroad flag and runs the script afterwards.

using SteadyClock = std::chrono::steady_clock;

struct DriveScriptRun {
  std::vector<std::string> args;
  SteadyClock::time_point not_before;
};

struct DriveWatch {
  bool driving = false;
  SteadyClock::time_point started_at;
  int64_t started_wall_ms = 0;
  std::string route;
  std::string last_route;  // the previous drive's, so a stale CurrentRoute is never taken for this one
  pid_t script_pid = -1;
  SteadyClock::time_point script_started;
  std::vector<DriveScriptRun> queued;
  SteadyClock::time_point next_totals;
  SteadyClock::time_point last_totals_request;
  SteadyClock::time_point last_position_run;
  std::optional<commaview::gps::Fix> last_fix;  // the comma's newest fix this drive, while location is on
  bool started = false;
};

std::mutex g_drive_mutex;
DriveWatch g_drive;

// Caller holds g_drive_mutex.
commaview::gps::GpsPeek& gps_peek_locked() {
  static commaview::gps::GpsPeek peek(msgq_dir());
  return peek;
}

int drive_poll_ms() {
#if !defined(__aarch64__)
  if (const char* test_ms = std::getenv("COMMAVIEWD_TEST_DRIVE_POLL_MS")) {
    const int ms = std::atoi(test_ms);
    if (ms > 0) return ms;
  }
#endif
  return 5000;
}

int after_drive_delay_sec() {
#if !defined(__aarch64__)
  if (std::getenv("COMMAVIEWD_TEST_DRIVE_SCRIPT") != nullptr) return 0;
#endif
  return kAfterDriveDelaySec;
}

// Where python3 is, searched now rather than in the child after fork.
std::string python_path() {
  if (access("/usr/local/venv/bin/python3", X_OK) == 0) return "/usr/local/venv/bin/python3";
  const char* path_env = std::getenv("PATH");
  std::stringstream dirs(path_env != nullptr ? path_env : "/usr/local/bin:/usr/bin:/bin");
  std::string dir;
  while (std::getline(dirs, dir, ':')) {
    const std::string candidate = (dir.empty() ? "." : dir) + "/python3";
    if (access(candidate.c_str(), X_OK) == 0) return candidate;
  }
  return "/usr/bin/python3";
}

// openpilot's own Python at the lowest priority, so the script reads the fork's logs and identity.
// Everything is prepared before fork: the child of this threaded process only makes async-signal-
// safe calls before exec.
pid_t spawn_drive_script(const std::vector<std::string>& args) {
  std::vector<std::string> argv;
  bool test_script = false;
#if !defined(__aarch64__)
  if (const char* script = std::getenv("COMMAVIEWD_TEST_DRIVE_SCRIPT")) {
    argv.push_back(script);
    test_script = true;
  }
#endif
  if (!test_script) {
    argv.push_back(python_path());
    argv.push_back(std::string(kInstallDir) + "/src/commaview_drive_stats.py");
  }
  argv.insert(argv.end(), args.begin(), args.end());
  std::vector<char*> argv_ptrs;
  for (auto& a : argv) argv_ptrs.push_back(const_cast<char*>(a.c_str()));
  argv_ptrs.push_back(nullptr);

  std::vector<std::string> env;
  for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
    if (!test_script && std::strncmp(*e, "PYTHONPATH=", 11) == 0) continue;
    env.emplace_back(*e);
  }
  if (!test_script) env.emplace_back("PYTHONPATH=/data/openpilot");
  std::vector<char*> env_ptrs;
  for (auto& e : env) env_ptrs.push_back(const_cast<char*>(e.c_str()));
  env_ptrs.push_back(nullptr);

  const std::string log_path = drivelog_path("logs/commaview-drive-stats.log");
  const char* workdir = test_script ? nullptr : "/data/openpilot";

  const pid_t pid = fork();
  if (pid != 0) return pid;  // the parent, or -1
  setsid();
  const int log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (log_fd >= 0) {
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);
  }
  if (workdir != nullptr && chdir(workdir) != 0) _exit(126);
  if (nice(19) == -1) {
    // Normal priority still beats not running.
  }
  execve(argv_ptrs[0], argv_ptrs.data(), env_ptrs.data());
  _exit(127);
}

// Caller holds g_drive_mutex.
void queue_drive_script_locked(std::vector<std::string> args, int delay_sec = 0) {
  for (const auto& run : g_drive.queued) {
    if (run.args == args) return;
  }
  g_drive.queued.push_back({std::move(args), SteadyClock::now() + std::chrono::seconds(delay_sec)});
}

void drive_watch_tick() {
  std::lock_guard<std::mutex> lk(g_drive_mutex);
  const auto now = SteadyClock::now();
  if (!g_drive.started) {
    g_drive.started = true;
    g_drive.next_totals = now + std::chrono::seconds(90);
  }
  if (g_drive.script_pid > 0) {
    int status = 0;
    if (waitpid(g_drive.script_pid, &status, WNOHANG) != 0) {
      g_drive.script_pid = -1;
    } else if (now - g_drive.script_started > std::chrono::seconds(kDriveScriptTimeoutSec)) {
      // A run stuck on the network (say) never holds up the next ones.
      kill(g_drive.script_pid, SIGKILL);
      waitpid(g_drive.script_pid, &status, 0);
      g_drive.script_pid = -1;
    }
  }

  const RoadState road = road_state();
  if (road == RoadState::kOnroad) {
    if (!g_drive.driving) {
      g_drive.driving = true;
      g_drive.started_at = now;
      const int64_t wall = wall_ms_now();
      g_drive.started_wall_ms = wall >= kValidWallMs ? wall : 0;
      g_drive.route.clear();
    }
    // loggerd names the route a moment after the drive starts; the latest new name is this drive's.
    const std::string route = read_param("CurrentRoute");
    if (!route.empty() && route != g_drive.last_route) g_drive.route = route;
    if (location_sharing_enabled()) {
      if (auto fix = gps_peek_locked().latest()) {
        if (g_drive.started_wall_ms == 0 || fix->fix_ms >= g_drive.started_wall_ms - 60000) g_drive.last_fix = fix;
      }
    } else {
      g_drive.last_fix.reset();
    }
  } else if (road == RoadState::kOffroad && g_drive.driving) {
    g_drive.driving = false;
    const int64_t duration_sec = std::chrono::duration_cast<std::chrono::seconds>(now - g_drive.started_at).count();
    // Where this drive ended: the comma's last fix, kept only while location is on.
    if (g_drive.last_fix && location_sharing_enabled()) {
      mkdir(drivelog_path("data").c_str(), 0700);
      write_file_atomic(drivelog_path("data/location-last.json"),
                        comma_fix_json(*g_drive.last_fix, ",\"endedMs\":" + std::to_string(wall_ms_now())) + "\n");
    }
    g_drive.last_fix.reset();
    if (duration_sec >= kMinDriveSec) {
      const int64_t end_wall = wall_ms_now();
      queue_drive_script_locked({"after-drive", "--route", g_drive.route,
                                 "--start-ms", std::to_string(g_drive.started_wall_ms),
                                 "--end-ms", std::to_string(end_wall >= kValidWallMs ? end_wall : 0),
                                 "--duration-s", std::to_string(duration_sec)},
                                after_drive_delay_sec());
      g_drive.next_totals = now + std::chrono::seconds(kTotalsRefreshSec);
    }
    if (!g_drive.route.empty()) g_drive.last_route = g_drive.route;
  }

  if (road == RoadState::kOffroad && now >= g_drive.next_totals) {
    queue_drive_script_locked({"totals"});
    g_drive.next_totals = now + std::chrono::seconds(kTotalsRefreshSec);
  }

  // One run at a time, in order. While the car is on only the position fallback runs, so reading a
  // drive's logs or asking comma never competes with driving; parked, a leftover position run is dropped.
  for (auto it = g_drive.queued.begin(); g_drive.script_pid <= 0 && it != g_drive.queued.end();) {
    const bool position_run = !it->args.empty() && it->args.front() == "position";
    if (road == RoadState::kOffroad && position_run) {
      it = g_drive.queued.erase(it);
      continue;
    }
    if (now < it->not_before || (road != RoadState::kOffroad && !position_run)) {
      ++it;
      continue;
    }
    const DriveScriptRun run = *it;
    g_drive.queued.erase(it);
    g_drive.script_pid = spawn_drive_script(run.args);
    g_drive.script_started = now;
    break;
  }
}

void start_drive_watcher() {
  std::thread([] {
    while (true) {
      drive_watch_tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(drive_poll_ms()));
    }
  }).detach();
}

// A phone looking at stats that are half an hour old asks for comma's totals again (while parked).
void request_totals_if_stale() {
  if (road_state() != RoadState::kOffroad) return;
  const int64_t updated_ms = file_mtime_ms(drivelog_path("data/drive-stats.json"));
  if (updated_ms > 0 && wall_ms_now() - updated_ms < static_cast<int64_t>(kTotalsStaleSec) * 1000) return;
  std::lock_guard<std::mutex> lk(g_drive_mutex);
  const auto now = SteadyClock::now();
  if (g_drive.last_totals_request.time_since_epoch().count() != 0 &&
      now - g_drive.last_totals_request < std::chrono::seconds(kTotalsRequestGapSec)) {
    return;
  }
  g_drive.last_totals_request = now;
  queue_drive_script_locked({"totals"});
}

std::string drive_stats_response_json() {
  request_totals_if_stale();
  std::string current = "null";
  {
    std::lock_guard<std::mutex> lk(g_drive_mutex);
    if (g_drive.driving) {
      const int64_t duration_sec = std::chrono::duration_cast<std::chrono::seconds>(SteadyClock::now() - g_drive.started_at).count();
      const int64_t wall = wall_ms_now();
      const int64_t start_ms = g_drive.started_wall_ms > 0 ? g_drive.started_wall_ms : (wall >= kValidWallMs ? wall - duration_sec * 1000 : 0);
      if (start_ms > 0) {
        current = "{\"startMs\":" + std::to_string(start_ms) + ",\"durationS\":" + std::to_string(duration_sec) + "}";
      }
    }
  }
  std::ostringstream out;
  out << "{\"ok\":true,\"onroad\":" << (road_state() == RoadState::kOnroad ? "true" : "false")
      << ",\"current\":" << current
      << ",\"ledger\":" << embedded_json_object(drivelog_path("data/drives.json"))
      << ",\"totals\":" << embedded_json_object(drivelog_path("data/drive-stats.json")) << "}";
  return out.str();
}

// While driving: sunnypilot's current position, else the comma's own newest GPS fix read from
// openpilot's msgq ring without subscribing. If neither answers (say msgq's layout changed), the
// position at the end of the drive's last finished log minute, asked for about once a minute.
std::string live_location_json() {
  const std::string sunnypilot = sunnypilot_position_json(memory_params_dir() + "/LastGPSPosition", kLivePositionMaxAgeSec);
  if (sunnypilot != "null") return sunnypilot;
  {
    std::lock_guard<std::mutex> lk(g_drive_mutex);
    if (auto fix = gps_peek_locked().latest()) {
      const int64_t age_ms = wall_ms_now() - fix->fix_ms;
      if (age_ms >= -5000 && age_ms <= static_cast<int64_t>(kLivePositionMaxAgeSec) * 1000) return comma_fix_json(*fix);
    }
    const auto now = SteadyClock::now();
    if (g_drive.driving && !g_drive.route.empty() &&
        (g_drive.last_position_run.time_since_epoch().count() == 0 ||
         now - g_drive.last_position_run >= std::chrono::seconds(kPositionRunGapSec))) {
      g_drive.last_position_run = now;
      queue_drive_script_locked({"position", "--route", g_drive.route});
    }
  }
  return embedded_json_object(drivelog_live_dir() + "/location-log.json");
}

// Where the last drive ended: from its log, else the position sunnypilot saves once a minute.
std::string last_location_json() {
  const std::string from_log = embedded_json_object(drivelog_path("data/location-last.json"));
  if (from_log != "null") return from_log;
  return sunnypilot_position_json(params_path("LastGPSPositionLLK"), 0);
}

std::string location_response_json() {
  const bool enabled = location_sharing_enabled();
  const bool onroad = road_state() == RoadState::kOnroad;
  std::ostringstream out;
  out << "{\"ok\":true,\"enabled\":" << (enabled ? "true" : "false")
      << ",\"onroad\":" << (onroad ? "true" : "false")
      << ",\"live\":" << (enabled && onroad ? live_location_json() : "null")
      << ",\"last\":" << (enabled ? last_location_json() : "null") << "}";
  return out.str();
}

std::string location_set_response(const std::string& body) {
  bool enabled = false;
  if (!extract_bool_field(body, "enabled", &enabled)) {
    return "{\"ok\":false,\"error\":\"enabled must be true or false\"}";
  }
  mkdir(drivelog_path("config").c_str(), 0755);
  if (!write_file_atomic(drivelog_path("config/location.json"),
                         enabled ? "{\"enabled\":true}\n" : "{\"enabled\":false}\n")) {
    return "{\"ok\":false,\"error\":\"Could not save the location setting\"}";
  }
  if (!enabled) {
    unlink((drivelog_live_dir() + "/location-log.json").c_str());
    unlink(drivelog_path("data/location-last.json").c_str());
  }
  return std::string("{\"ok\":true,\"enabled\":") + (enabled ? "true" : "false") + "}";
}

bool extract_pair_code(const std::string& body, std::string* code_out) {
  return extract_raw_string_field(body, "pairCode", code_out) ||
         extract_raw_string_field(body, "code", code_out);
}

std::string pairing_create(const std::string& api_token) {
  if (api_token.empty()) {
    return "{\"ok\":false,\"error\":\"api token unavailable\"}";
  }
  const std::string code = random_pair_code();
  {
    std::lock_guard<std::mutex> lk(g_pairing_mutex);
    g_pairing_grant.code = code;
    g_pairing_grant.expires_at = std::time(nullptr) + kPairingCodeTtlSec;
    g_pairing_grant.used = false;
  }

  std::ostringstream out;
  out << "{\"ok\":true,\"pairCode\":\"" << code
      << "\",\"expiresInSec\":" << kPairingCodeTtlSec
      << ",\"pairingUri\":\"" << kPairingScheme << "?code=" << code << "\"}";
  return out.str();
}

std::string pairing_redeem(const std::string& code, const std::string& api_token) {
  if (api_token.empty()) {
    return "{\"ok\":false,\"error\":\"api token unavailable\"}";
  }
  if (code.empty()) {
    return "{\"ok\":false,\"error\":\"pairCode required\"}";
  }

  std::lock_guard<std::mutex> lk(g_pairing_mutex);
  const std::time_t now = std::time(nullptr);
  if (g_pairing_grant.code.empty() || g_pairing_grant.used) {
    return "{\"ok\":false,\"error\":\"pairing code unavailable\"}";
  }
  if (now > g_pairing_grant.expires_at) {
    g_pairing_grant.used = true;
    return "{\"ok\":false,\"error\":\"pairing code expired\"}";
  }
  if (!codes_equal(code, g_pairing_grant.code)) {
    return "{\"ok\":false,\"error\":\"invalid pairing code\"}";
  }

  g_pairing_grant.used = true;
  return std::string("{\"ok\":true,\"apiToken\":\"") + json_escape(api_token) + "\"}";
}

bool is_authorized(const commaview::api::HttpRequest& req, const std::string& token) {
  if (token.empty()) return true;
  auto it = req.headers.find("x-commaview-token");
  if (it == req.headers.end()) return false;
  return it->second == token;
}

commaview::api::HttpResponse make_json(int code, const std::string& body) {
  commaview::api::HttpResponse resp;
  resp.status = code;
  resp.body = body;
  return resp;
}

std::string onroad_ui_export_status_response() {
  return live_onroad_ui_export_status_json(false);
}

std::string onroad_ui_export_repair_response(const std::string& request_body) {
  const bool force_offroad = json_field_true(request_body, "forceOffroad");
  int rc = 0;
  std::string err;
  const std::string status = run_onroad_ui_export_apply_status_json(&rc, &err, force_offroad);
  std::ostringstream resp;
  resp << "{\"ok\":" << (rc == 0 ? "true" : "false") << ",\"repairNeeded\":" << (rc == 0 ? "false" : "true") << ",\"status\":" << status;
  if (rc != 0) {
    resp << ",\"error\":\"" << json_escape(err.empty() ? "onroad UI export repair failed" : err) << "\"";
  }
  resp << "}";
  return resp.str();
}

constexpr const char* kUnauthorizedJson = "{\"ok\":false,\"error\":\"unauthorized\"}";

std::string version_response_json() {
  const std::string version = runtime_version();
  const std::string telemetryMode = telemetry_mode();
  const std::string dongleId = device_dongle_id();
  const std::string hardwareSerial = device_hardware_serial();
  const std::string model = device_model();
  std::ostringstream body;
  body << "{\"version\":\"" << json_escape(version) << "\",";
  body << "\"runtimeVersion\":\"" << json_escape(version) << "\",";
  body << "\"dongleId\":\"" << json_escape(dongleId) << "\",";
  body << "\"dongle_id\":\"" << json_escape(dongleId) << "\",";
  body << "\"hardwareSerial\":\"" << json_escape(hardwareSerial) << "\",";
  body << "\"deviceModel\":\"" << json_escape(model) << "\",";
  body << "\"device\":\"" << json_escape(model) << "\",";
  body << "\"api_port\":" << kDefaultApiPort << ",";
  body << "\"telemetryMode\":\"" << json_escape(telemetryMode) << "\"}";
  return body.str();
}

commaview::api::HttpResponse handle_source_recording_get(const commaview::api::HttpRequest& req,
                                                         const std::string& api_token) {
  // Route archives and UI timelines are private, even on devices where
  // ordinary control endpoints have not yet been paired.
  if (api_token.empty() || !is_authorized(req, api_token)) {
    return make_json(401, kUnauthorizedJson);
  }
  if (req.path == "/commaview/source-recording/current") {
    if (!is_onroad()) {
      return make_json(403, "{\"ok\":false,\"error\":\"onroad required\"}");
    }
    return source_recording_current_response(read_param("CurrentRoute"));
  }
  if (is_onroad()) {
    return make_json(403, "{\"ok\":false,\"error\":\"offroad required\"}");
  }
  return source_recording_archive_response(req.path);
}

commaview::api::HttpResponse handle_get(const commaview::api::HttpRequest& req, const std::string& api_token) {
  if (req.path.rfind("/commaview/source-recording/", 0) == 0) {
    return handle_source_recording_get(req, api_token);
  }
  if (req.path == "/commaview/version") {
    return make_json(200, version_response_json());
  }
  if (req.path == "/commaview/status") {
    return make_json(200, runtime_status_json());
  }
  if (req.path == "/commaview/onroad-ui-export/status") {
    return make_json(200, onroad_ui_export_status_response());
  }
  if (req.path == "/commaview/runtime-debug/config") {
    return make_json(200, runtime_debug_state_json());
  }
  if (req.path == "/commaview/wifi/power-save") {
    if (!is_authorized(req, api_token)) {
      return make_json(401, kUnauthorizedJson);
    }
    const std::string body = wifi_power_save_status_response();
    return make_json(body.find("\"ok\":true") != std::string::npos ? 200 : 503, body);
  }
  if (req.path == "/commaview/support/logs") {
    if (!is_authorized(req, api_token)) {
      return make_json(401, kUnauthorizedJson);
    }
    return make_json(200, support_logs_response_json());
  }
  if (req.path == "/commaview/drive-stats" || req.path == "/commaview/location") {
    // Where and how this comma drives is private: only a paired phone may ask.
    if (api_token.empty() || !is_authorized(req, api_token)) {
      return make_json(401, kUnauthorizedJson);
    }
    return make_json(200, req.path == "/commaview/location" ? location_response_json() : drive_stats_response_json());
  }
  return make_json(404, "{\"error\":\"not found\"}");
}

commaview::api::HttpResponse handle_post(const commaview::api::HttpRequest& req, const std::string& api_token) {
  if (req.path == "/pairing/redeem") {
    std::string code;
    if (!extract_pair_code(req.body, &code)) {
      return make_json(400, "{\"ok\":false,\"error\":\"pairCode required\"}");
    }
    std::string body = pairing_redeem(code, api_token);
    int status = body.find("\"ok\":true") != std::string::npos ? 200 : 400;
    return make_json(status, body);
  }

  // Everything below /pairing/redeem needs the token, including unknown paths.
  if (!is_authorized(req, api_token)) {
    return make_json(401, kUnauthorizedJson);
  }

  if (req.path == "/pairing/create") {
    return make_json(200, pairing_create(api_token));
  }

  if (req.path == "/commaview/location") {
    if (api_token.empty()) {
      return make_json(401, kUnauthorizedJson);
    }
    const std::string body = location_set_response(req.body);
    return make_json(body.find("\"ok\":true") != std::string::npos ? 200 : 400, body);
  }

  if (req.path == "/commaview/wifi/power-save") {
    const std::string body = wifi_power_save_set_response(req.body);
    const int code = body.find("\"ok\":true") != std::string::npos ? 200 :
                     body.find("offroad and disengaged required") != std::string::npos ? 403 : 400;
    return make_json(code, body);
  }

  if (req.path == "/commaview/runtime-debug/config") {
    commaview::runtime_debug::LoadedRuntimeDebugConfig parsed;
    std::string error;
    if (!write_runtime_debug_config_json(req.body, &parsed, &error)) {
      return make_json(400, runtime_debug_write_response(false, load_persisted_runtime_debug_config(), error));
    }
    return make_json(200, runtime_debug_write_response(true, parsed));
  }
  if (req.path == "/commaview/runtime-debug/defaults") {
    std::string body = runtime_debug_restore_defaults_response();
    int code = body.find("\"ok\":true") != std::string::npos ? 200 : 500;
    return make_json(code, body);
  }
  if (req.path == "/commaview/runtime-debug/apply") {
    std::string body = runtime_debug_apply_response();
    int code = body.find("\"ok\":true") != std::string::npos ? 200 : 500;
    return make_json(code, body);
  }
  if (req.path == "/commaview/onroad-ui-export/repair") {
    std::string body = onroad_ui_export_repair_response(req.body);
    int code = body.find("\"ok\":true") != std::string::npos ? 200 : 500;
    return make_json(code, body);
  }
  return make_json(404, "{\"error\":\"not found\"}");
}

commaview::api::HttpResponse handle_request(const commaview::api::HttpRequest& req, const std::string& api_token) {
  if (req.method == "OPTIONS") {
    commaview::api::HttpResponse r;
    r.status = 204;
    r.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS";
    return r;
  }
  if (req.method == "GET") return handle_get(req, api_token);
  if (req.method == "POST") return handle_post(req, api_token);
  return make_json(405, "{\"error\":\"method not allowed\"}");
}

}  // namespace

int run_control_mode(int argc, char* argv[]) {
  int port = kDefaultApiPort;
  for (int i = 2; i < argc; i++) {
    if (std::strcmp(argv[i], "--port") == 0 && (i + 1) < argc) {
      port = std::atoi(argv[i + 1]);
      i++;
    }
  }

  const std::string api_token = load_api_token();

  commaview::api::HttpServer server(port, [api_token](const commaview::api::HttpRequest& req) {
    return handle_request(req, api_token);
  });

  std::string err;
  if (!server.start(&err)) {
    std::fprintf(stderr, "commaviewd control: failed to start server on :%d (%s)\n", port, err.c_str());
    return 2;
  }

  std::printf("commaviewd control: listening on :%d\n", port);
  std::fflush(stdout);
  start_discovery_responder();
  start_drive_watcher();

  server.serve_forever();
  return 0;
}

}  // namespace commaview::runtime
