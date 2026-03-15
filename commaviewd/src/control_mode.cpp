#include "control_mode.h"
#include "http_server.h"
#include "policy.h"

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

namespace commaview::runtime {
namespace {

constexpr const char* kInstallDir = "/data/commaview";
constexpr const char* kParamsDir = "/data/params/d";
constexpr const char* kTailscaleDir = "/data/commaview/tailscale";
constexpr const char* kTailscaleBin = "/data/commaview/tailscale/bin/tailscale";
constexpr const char* kTailscaledBin = "/data/commaview/tailscale/bin/tailscaled";
constexpr const char* kTailscaleSocket = "/data/commaview/tailscale/state/tailscaled.sock";
constexpr const char* kTailscaleStateFile = "/data/commaview/tailscale/state/tailscaled.state";
constexpr const char* kTailscaleAuthKeyFile = "/data/commaview/tailscale/authkey";
constexpr const char* kTailscaleRuntimeInstaller = "/data/commaview/tailscale/install_tailscale_runtime.sh";
constexpr const char* kControlLogFile = "/data/commaview/logs/commaviewd-control.log";
constexpr const char* kSetupCompleteParam = "CommaViewTailscaleSetupComplete";
constexpr int kDefaultApiPort = 5002;
constexpr const char* kVersionFile = "/data/commaview/VERSION";

struct TailscaleSnapshot {
  bool enabled = false;
  bool onroad = false;
  bool daemon_running = false;
  bool auth_key_pending = false;
  std::string backend_state = "unknown";
};

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
    const char* env = std::getenv("COMMAVIEWD_TELEMETRY_MODE");
    std::string mode = env ? trim_copy(env) : "raw-only";
    if (mode.empty()) return "raw-only";
    if (mode == "raw-only" || mode == "json-only" || mode == "raw+json") return mode;
    return "raw-only";
}

bool write_file(const std::string& path, const std::string& value, mode_t mode = 0644) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) return false;
  const ssize_t want = static_cast<ssize_t>(value.size());
  const ssize_t wrote = write(fd, value.data(), static_cast<size_t>(want));
  close(fd);
  return wrote == want;
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

bool read_setup_complete() {
  return read_param(kSetupCompleteParam) == "1";
}

bool write_setup_complete(bool value) {
  return write_param(kSetupCompleteParam, value ? "1" : "0");
}

bool extract_setup_complete_flag(const std::string& body, bool* value_out) {
  if (value_out == nullptr) return false;
  size_t pos = body.find("\"setupComplete\"");
  if (pos == std::string::npos) return false;
  pos = body.find(':', pos);
  if (pos == std::string::npos) return false;
  pos++;
  while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) pos++;
  if (pos >= body.size()) return false;

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

std::string setup_complete_json(bool value) {
  return std::string("{\"setupComplete\":") + (value ? "true" : "false") + "}";
}

std::string setup_complete_write_json(bool ok, bool value, const std::string& error = "") {
  std::ostringstream out;
  out << "{\"ok\":" << (ok ? "true" : "false")
      << ",\"setupComplete\":" << (value ? "true" : "false");
  if (!error.empty()) {
    out << ",\"error\":\"" << json_escape(error) << "\"";
  }
  out << "}";
  return out.str();
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

bool ensure_tailscale_runtime_available(std::string* error_out) {
  if (file_executable(kTailscaleBin) && file_executable(kTailscaledBin)) {
    return true;
  }
  if (!file_executable(kTailscaleRuntimeInstaller)) {
    if (error_out) *error_out = "tailscale runtime helper missing";
    return false;
  }

  int rc = 0;
  std::string out;
  std::string err;
  if (!run_command_with_optional_sudo({kTailscaleRuntimeInstaller}, &rc, &out, &err)) {
    std::string msg = trim_copy(err.empty() ? out : err);
    if (msg.empty()) msg = "tailscale runtime install failed";
    if (error_out) *error_out = msg;
    return false;
  }

  if (!file_executable(kTailscaleBin) || !file_executable(kTailscaledBin)) {
    if (error_out) *error_out = "tailscale runtime still missing after install";
    return false;
  }

  return true;
}

bool tailscaled_running() {
  int rc = 0;
  std::string out;
  std::string err;
  if (!run_command({"pgrep", "-f", kTailscaledBin}, &rc, &out, &err)) return false;
  return rc == 0;
}

bool start_tailscaled(std::string* error_out = nullptr) {
  if (!file_executable(kTailscaledBin)) {
    if (error_out) *error_out = "tailscaled binary missing";
    return false;
  }
  if (tailscaled_running()) return true;

  mkdir("/data/commaview/logs", 0755);
  mkdir("/data/commaview/tailscale", 0755);
  mkdir("/data/commaview/tailscale/state", 0755);

  pid_t pid = fork();
  if (pid < 0) {
    if (error_out) *error_out = "fork failed";
    return false;
  }

  if (pid == 0) {
    setsid();

    int logfd = open(kControlLogFile, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (logfd >= 0) {
      dup2(logfd, STDOUT_FILENO);
      dup2(logfd, STDERR_FILENO);
      close(logfd);
    }

    std::string state_arg = std::string("--state=") + kTailscaleStateFile;
    std::string socket_arg = std::string("--socket=") + kTailscaleSocket;
    if (geteuid() == 0) {
      execl(kTailscaledBin, kTailscaledBin, state_arg.c_str(), socket_arg.c_str(), static_cast<char*>(nullptr));
    } else {
      execlp("sudo", "sudo", "-n", kTailscaledBin, state_arg.c_str(), socket_arg.c_str(), static_cast<char*>(nullptr));
    }
    _exit(127);
  }

  sleep(1);
  if (tailscaled_running()) return true;
  if (error_out) *error_out = "failed to start tailscaled (permission denied or sudo unavailable)";
  return false;
}

void force_tailscale_down_and_stop() {
  if (file_executable(kTailscaleBin)) {
    int rc = 0;
    std::string out;
    std::string err;
    (void)run_command({kTailscaleBin, "--socket", kTailscaleSocket, "down"}, &rc, &out, &err);
  }
  int rc = 0;
  std::string out;
  std::string err;
  (void)run_command({"pkill", "-f", kTailscaledBin}, &rc, &out, &err);
}

std::string parse_backend_state(const std::string& status_json) {
  const std::string key = "\"BackendState\"";
  size_t pos = status_json.find(key);
  if (pos == std::string::npos) return "unknown";
  pos = status_json.find(':', pos + key.size());
  if (pos == std::string::npos) return "unknown";
  pos = status_json.find('"', pos + 1);
  if (pos == std::string::npos) return "unknown";
  size_t end = status_json.find('"', pos + 1);
  if (end == std::string::npos) return "unknown";
  return status_json.substr(pos + 1, end - (pos + 1));
}

std::string tailscale_backend_state() {
  if (!file_executable(kTailscaleBin)) return "missing";
  int rc = 0;
  std::string out;
  std::string err;
  if (!run_command({kTailscaleBin, "--socket", kTailscaleSocket, "status", "--json"}, &rc, &out, &err)) {
    return "error";
  }
  if (rc != 0) return "error";
  const std::string parsed = parse_backend_state(out);
  return parsed.empty() ? "unknown" : parsed;
}

bool authkey_pending() {
  struct stat st{};
  return stat(kTailscaleAuthKeyFile, &st) == 0 && st.st_size > 0;
}

std::string read_authkey() {
  return read_file_trimmed(kTailscaleAuthKeyFile);
}

bool write_authkey(const std::string& key) {
  mkdir(kTailscaleDir, 0755);
  return write_file(kTailscaleAuthKeyFile, key, 0600);
}

TailscaleSnapshot capture_tailscale_snapshot() {
  TailscaleSnapshot s;
  s.enabled = read_param("CommaViewTailscaleEnabled") == "1";
  s.onroad = is_onroad();
  s.daemon_running = tailscaled_running();
  s.backend_state = tailscale_backend_state();
  s.auth_key_pending = authkey_pending();
  return s;
}

std::string snapshot_json(const TailscaleSnapshot& s) {
  std::ostringstream out;
  out << "{"
      << "\"enabled\":" << (s.enabled ? "true" : "false") << ","
      << "\"onroad\":" << (s.onroad ? "true" : "false") << ","
      << "\"daemonRunning\":" << (s.daemon_running ? "true" : "false") << ","
      << "\"backendState\":\"" << json_escape(s.backend_state) << "\","
      << "\"authKeyPending\":" << (s.auth_key_pending ? "true" : "false")
      << "}";
  return out.str();
}

std::string command_result_json(bool ok, bool available, const TailscaleSnapshot& snap, const std::string& error = "") {
  std::ostringstream out;
  out << "{"
      << "\"ok\":" << (ok ? "true" : "false") << ","
      << "\"available\":" << (available ? "true" : "false") << ","
      << "\"enabled\":" << (snap.enabled ? "true" : "false") << ","
      << "\"onroad\":" << (snap.onroad ? "true" : "false") << ","
      << "\"daemonRunning\":" << (snap.daemon_running ? "true" : "false") << ","
      << "\"backendState\":\"" << json_escape(snap.backend_state) << "\","
      << "\"authKeyPending\":" << (snap.auth_key_pending ? "true" : "false");
  if (!error.empty()) {
    out << ",\"error\":\"" << json_escape(error) << "\"";
  }
  out << "}";
  return out.str();
}

std::string tailscale_status() {
  if (!file_executable(kTailscaleBin) || !file_executable(kTailscaledBin)) {
    return "{\"enabled\":false,\"onroad\":false,\"daemonRunning\":false,\"backendState\":\"missing\",\"authKeyPending\":false,\"available\":false}";
  }
  return snapshot_json(capture_tailscale_snapshot());
}

std::string tailscale_set_enabled(bool enable) {
  if (enable) {
    std::string runtime_error;
    if (!ensure_tailscale_runtime_available(&runtime_error)) {
      if (runtime_error.empty()) runtime_error = "tailscale runtime missing";
      return command_result_json(false, false, capture_tailscale_snapshot(), runtime_error);
    }
  }

  const bool available = file_executable(kTailscaleBin) && file_executable(kTailscaledBin);
  if (!available) {
    return command_result_json(false, false, capture_tailscale_snapshot(), "tailscale runtime missing");
  }

  const auto action = commaview::control::decide_tailscale_action(is_onroad(), enable);
  if (action == commaview::control::TailscalePolicyAction::kForceDown) {
    write_param("CommaViewTailscaleEnabled", "0");
    force_tailscale_down_and_stop();
    return command_result_json(false, true, capture_tailscale_snapshot(), "onroad: tailscale forced down");
  }

  if (!enable) {
    write_param("CommaViewTailscaleEnabled", "0");
    force_tailscale_down_and_stop();
    return command_result_json(true, true, capture_tailscale_snapshot());
  }

  write_param("CommaViewTailscaleEnabled", "1");
  std::string start_error;
  if (!start_tailscaled(&start_error)) {
    if (start_error.empty()) start_error = "failed to start tailscaled";
    return command_result_json(false, true, capture_tailscale_snapshot(), start_error);
  }

  std::vector<std::string> args = {kTailscaleBin, "--socket", kTailscaleSocket, "up"};
  std::string authkey = read_authkey();
  if (!authkey.empty()) {
    args.push_back("--authkey");
    args.push_back(authkey);
    args.push_back("--accept-routes");
    args.push_back("--netfilter-mode=off");
    args.push_back("--reset");
  } else {
    args.push_back("--accept-routes");
    args.push_back("--netfilter-mode=off");
  }

  if (geteuid() != 0) {
    const char* operator_user = std::getenv("USER");
    if (operator_user != nullptr && std::strlen(operator_user) > 0) {
      args.push_back("--operator");
      args.push_back(operator_user);
    }
  }

  int rc = 0;
  std::string out;
  std::string err;
  if (!run_command_with_optional_sudo(args, &rc, &out, &err)) {
    return command_result_json(false, true, capture_tailscale_snapshot(), "tailscale up spawn failed");
  }
  if (rc != 0) {
    std::string msg = trim_copy(err.empty() ? out : err);
    if (msg.empty()) msg = "tailscale up failed";
    return command_result_json(false, true, capture_tailscale_snapshot(), msg);
  }

  if (!authkey.empty()) {
    unlink(kTailscaleAuthKeyFile);
  }
  return command_result_json(true, true, capture_tailscale_snapshot());
}

bool extract_auth_key(const std::string& body, std::string* authkey_out) {
  if (authkey_out == nullptr) return false;
  const char* keys[] = {"\"authKey\"", "\"auth_key\""};

  for (const char* key : keys) {
    size_t pos = body.find(key);
    if (pos == std::string::npos) continue;
    pos = body.find(':', pos);
    if (pos == std::string::npos) continue;
    pos++;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) pos++;
    if (pos >= body.size() || body[pos] != '"') continue;
    pos++;
    size_t end = body.find('"', pos);
    if (end == std::string::npos) continue;
    *authkey_out = body.substr(pos, end - pos);
    return !authkey_out->empty();
  }

  return false;
}

std::string tailscale_set_authkey(const std::string& authkey) {
  std::string runtime_error;
  if (!ensure_tailscale_runtime_available(&runtime_error)) {
    if (runtime_error.empty()) runtime_error = "tailscale runtime missing";
    return command_result_json(false, false, capture_tailscale_snapshot(), runtime_error);
  }

  const bool available = file_executable(kTailscaleBin) && file_executable(kTailscaledBin);
  if (!available) {
    return command_result_json(false, false, capture_tailscale_snapshot(), "tailscale runtime missing");
  }
  if (authkey.empty()) {
    return command_result_json(false, true, capture_tailscale_snapshot(), "auth key required");
  }
  if (!write_authkey(authkey)) {
    return command_result_json(false, true, capture_tailscale_snapshot(), "failed to write auth key");
  }
  return command_result_json(true, true, capture_tailscale_snapshot());
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
      if (req.path == "/tailscale/status") {
        return make_json(200, tailscale_status());
      }
      if (req.path == "/tailscale/setup-complete") {
        return make_json(200, setup_complete_json(read_setup_complete()));
      }
      if (req.path == "/commaview/version") {
        const std::string version = runtime_version();
        return make_json(200, "{\"version\":\"" + json_escape(version) + "\"}");
      }
      if (req.path == "/commaview/status") {
        const std::string version = runtime_version();
        const std::string telemetryMode = telemetry_mode();
        std::string body = "{\"version\":\"" + json_escape(version) + "\",\"api_port\":5002,\"telemetryMode\":\"" + json_escape(telemetryMode) + "\",\"tailscale\":" + tailscale_status() + "}";
        return make_json(200, body);
      }
      return make_json(404, "{\"error\":\"not found\"}");
    }

    if (req.method == "POST") {
      if (!is_authorized(req, api_token)) {
        return make_json(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
      }

      if (req.path == "/tailscale/enable") {
        std::string body = tailscale_set_enabled(true);
        int code = body.find("\"ok\":false") != std::string::npos ? 500 : 200;
        return make_json(code, body);
      }
      if (req.path == "/tailscale/disable") {
        std::string body = tailscale_set_enabled(false);
        int code = body.find("\"ok\":false") != std::string::npos ? 500 : 200;
        return make_json(code, body);
      }
      if (req.path == "/tailscale/authkey") {
        std::string authkey;
        if (!extract_auth_key(req.body, &authkey)) {
          return make_json(400, "{\"ok\":false,\"error\":\"auth key required\"}");
        }
        std::string body = tailscale_set_authkey(authkey);
        int code = body.find("\"ok\":false") != std::string::npos ? 500 : 200;
        return make_json(code, body);
      }
      if (req.path == "/tailscale/setup-complete") {
        bool setup_complete = false;
        if (!extract_setup_complete_flag(req.body, &setup_complete)) {
          return make_json(400, "{\"ok\":false,\"error\":\"setupComplete required\"}");
        }
        if (!write_setup_complete(setup_complete)) {
          return make_json(500, setup_complete_write_json(false, read_setup_complete(), "failed to write setupComplete"));
        }
        return make_json(200, setup_complete_write_json(true, read_setup_complete()));
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
