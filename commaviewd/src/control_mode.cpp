#include "control_mode.h"
#include "http_server.h"
#include "runtime_debug_config.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

namespace commaview::runtime {
namespace {

constexpr const char* kInstallDir = "/data/commaview";
constexpr const char* kParamsDir = "/data/params/d";
constexpr int kDefaultApiPort = 5002;
constexpr const char* kVersionFile = "/data/commaview/VERSION";
constexpr const char* kPairingScheme = "commaview://pair";
constexpr int kPairingCodeTtlSec = 300;
constexpr const char* kOnroadUiExportStatusFile = "/data/commaview/run/onroad-ui-export-status.json";
constexpr const char* kOnroadUiExportApplyScript = "/data/commaview/scripts/apply_onroad_ui_export_patch.sh";
constexpr const char* kOnroadUiExportVerifyScript = "/data/commaview/scripts/verify_onroad_ui_export_patch.sh";

struct PairingGrant {
  std::string code;
  std::time_t expires_at = 0;
  bool used = true;
};

std::mutex g_pairing_mutex;
PairingGrant g_pairing_grant;
bool run_command(const std::vector<std::string>& args, int* exit_code, std::string* stdout_text, std::string* stderr_text);
bool is_onroad();

std::string trim_copy(const std::string& in) {
  size_t s = 0;
  while (s < in.size() && std::isspace(static_cast<unsigned char>(in[s]))) s++;
  size_t e = in.size();
  while (e > s && std::isspace(static_cast<unsigned char>(in[e - 1]))) e--;
  return in.substr(s, e - s);
}

std::string json_escape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

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
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    ssize_t n = read(fd, bytes.data(), bytes.size());
    close(fd);
    if (n < static_cast<ssize_t>(bytes.size())) {
      std::srand(static_cast<unsigned int>(std::time(nullptr) ^ getpid()));
      for (size_t i = static_cast<size_t>(n < 0 ? 0 : n); i < bytes.size(); ++i) {
        bytes[i] = static_cast<unsigned char>(std::rand() & 0xFF);
      }
    }
  } else {
    std::srand(static_cast<unsigned int>(std::time(nullptr) ^ getpid()));
    for (size_t i = 0; i < bytes.size(); ++i) {
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

std::string read_file_trimmed(const std::string& path) {
  std::ifstream f(path);
  if (!f) return "";
  std::stringstream ss;
  ss << f.rdbuf();
  return trim_copy(ss.str());
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

std::string read_file_raw(const std::string& path) {
  std::ifstream f(path);
  if (!f) return "";
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
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
  const std::string raw = trim_copy(read_file_raw(commaview::runtime_debug::runtime_debug_stats_path()));
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

std::string default_onroad_ui_export_status_json() {
  return "{\"healthy\":false,\"patchVerified\":false,\"statusScope\":\"patch-installation\",\"repairNeeded\":true,\"state\":\"missing\",\"reason\":\"onroad UI export status unavailable\"}";
}

std::string load_onroad_ui_export_status_json() {
  const std::string raw = trim_copy(read_file_raw(kOnroadUiExportStatusFile));
  return raw.empty() ? default_onroad_ui_export_status_json() : raw;
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
  if (!run_command(args, &rc, &out, &err)) {
    if (err_out) *err_out = trim_copy(err).empty() ? "onroad UI export repair failed" : trim_copy(err);
    return onroad_ui_export_status_error_json("error", trim_copy(err).empty() ? "onroad UI export repair failed" : trim_copy(err));
  }

  if (rc_out) *rc_out = rc;
  if (err_out) *err_out = trim_copy(err);

  const std::string body = trim_copy(out);
  if (!body.empty()) return body;
  if (rc == 0) return load_onroad_ui_export_status_json();
  return onroad_ui_export_status_error_json("error", trim_copy(err).empty() ? "onroad UI export repair failed" : trim_copy(err));
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

std::string runtime_status_json() {
  const std::string version = runtime_version();
  const std::string telemetryMode = telemetry_mode();
  const std::string dongleId = device_dongle_id();
  const std::string hardwareSerial = device_hardware_serial();
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
  return read_file_trimmed(std::string(kParamsDir) + "/" + key);
}

bool write_param(const char* key, const char* value) {
  mkdir(kParamsDir, 0755);
  const std::string path = std::string(kParamsDir) + "/" + key;
  return write_file(path, value, 0644);
}

bool is_onroad() {
  return read_param("IsOnroad") == "1";
}

bool extract_string_field(const std::string& body, const char* key, std::string* value_out) {
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

bool extract_pair_code(const std::string& body, std::string* code_out) {
  return extract_string_field(body, "pairCode", code_out) ||
         extract_string_field(body, "code", code_out);
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

}  // namespace

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
    if (req.method == "OPTIONS") {
      commaview::api::HttpResponse r;
      r.status = 204;
      r.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS";
      return r;
    }

    if (req.method == "GET") {
      if (req.path == "/commaview/version") {
        const std::string version = runtime_version();
        const std::string telemetryMode = telemetry_mode();
        const std::string dongleId = device_dongle_id();
        const std::string hardwareSerial = device_hardware_serial();
        std::ostringstream body;
        body << "{\"version\":\"" << json_escape(version) << "\",";
        body << "\"runtimeVersion\":\"" << json_escape(version) << "\",";
        body << "\"dongleId\":\"" << json_escape(dongleId) << "\",";
        body << "\"dongle_id\":\"" << json_escape(dongleId) << "\",";
        body << "\"hardwareSerial\":\"" << json_escape(hardwareSerial) << "\",";
        body << "\"api_port\":" << kDefaultApiPort << ",";
        body << "\"telemetryMode\":\"" << json_escape(telemetryMode) << "\"}";
        return make_json(200, body.str());
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
      return make_json(404, "{\"error\":\"not found\"}");
    }

    if (req.method == "POST") {
      if (req.path == "/pairing/redeem") {
        std::string code;
        if (!extract_pair_code(req.body, &code)) {
          return make_json(400, "{\"ok\":false,\"error\":\"pairCode required\"}");
        }
        std::string body = pairing_redeem(code, api_token);
        int status = body.find("\"ok\":true") != std::string::npos ? 200 : 400;
        return make_json(status, body);
      }

      if (!is_authorized(req, api_token)) {
        return make_json(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
      }

      if (req.path == "/pairing/create") {
        return make_json(200, pairing_create(api_token));
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

    return make_json(405, "{\"error\":\"method not allowed\"}");
  });

  std::string err;
  if (!server.start(&err)) {
    std::fprintf(stderr, "commaviewd control: failed to start server on :%d (%s)\n", port, err.c_str());
    return 2;
  }

  std::printf("commaviewd control: listening on :%d\n", port);
  std::fflush(stdout);

  server.serve_forever();
  return 0;
}

}  // namespace commaview::runtime
