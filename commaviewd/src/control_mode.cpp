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
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace commaview::runtime {
namespace {

constexpr const char* kInstallDir = "/data/commaview";
constexpr const char* kTailscaleCtl = "/data/commaview/tailscale/tailscalectl.sh";
constexpr int kDefaultApiPort = 5002;

volatile std::sig_atomic_t g_stop = 0;

void signal_handler(int) {
  g_stop = 1;
}

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

std::string read_file_trimmed(const std::string& path) {
  std::ifstream f(path);
  if (!f) return "";
  std::stringstream ss;
  ss << f.rdbuf();
  return trim_copy(ss.str());
}

std::string read_param(const char* key) {
  return read_file_trimmed(std::string("/data/params/d/") + key);
}

bool is_onroad() {
  return read_param("IsOnroad") == "1";
}

std::string load_api_token() {
  const char* direct = std::getenv("COMMAVIEW_API_TOKEN");
  if (direct != nullptr) {
    std::string token = trim_copy(direct);
    if (!token.empty()) return token;
  }

  const char* token_file_env = std::getenv("COMMAVIEW_API_TOKEN_FILE");
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

std::string tailscale_status() {
  if (access(kTailscaleCtl, X_OK) != 0) {
    return "{\"enabled\":false,\"onroad\":false,\"daemonRunning\":false,\"backendState\":\"missing\",\"available\":false}";
  }

  int rc = 0;
  std::string out;
  std::string err;
  if (!run_command({kTailscaleCtl, "status", "--json"}, &rc, &out, &err)) {
    return "{\"enabled\":false,\"onroad\":false,\"daemonRunning\":false,\"backendState\":\"error\",\"available\":true,\"error\":\"spawn failed\"}";
  }

  out = trim_copy(out);
  if (rc != 0) {
    std::string msg = trim_copy(err.empty() ? out : err);
    if (msg.empty()) msg = "tailscalectl status failed";
    return "{\"enabled\":false,\"onroad\":false,\"daemonRunning\":false,\"backendState\":\"error\",\"available\":true,\"error\":\"" + json_escape(msg) + "\"}";
  }

  if (out.empty()) {
    return "{\"enabled\":false,\"onroad\":false,\"daemonRunning\":false,\"backendState\":\"unknown\",\"available\":true}";
  }

  return out;
}

std::string tailscale_set_enabled(bool enable) {
  if (access(kTailscaleCtl, X_OK) != 0) {
    return "{\"ok\":false,\"error\":\"tailscalectl missing\",\"available\":false}";
  }

  const auto action = commaview::control::decide_tailscale_action(is_onroad(), enable);
  if (action == commaview::control::TailscalePolicyAction::kForceDown) {
    // Hard policy gate: never allow enabling while onroad.
    (void)run_command({kTailscaleCtl, "disable", "--json"}, nullptr, nullptr, nullptr);
    return "{\"ok\":false,\"available\":true,\"error\":\"onroad: tailscale forced down\"}";
  }

  int rc = 0;
  std::string out;
  std::string err;
  const char* cmd = enable ? "enable" : "disable";
  if (!run_command({kTailscaleCtl, cmd, "--json"}, &rc, &out, &err)) {
    return "{\"ok\":false,\"available\":true,\"error\":\"spawn failed\"}";
  }

  out = trim_copy(out);
  if (rc != 0) {
    std::string msg = trim_copy(err.empty() ? out : err);
    if (msg.empty()) msg = std::string("tailscalectl ") + cmd + " failed";
    return "{\"ok\":false,\"available\":true,\"error\":\"" + json_escape(msg) + "\"}";
  }

  if (out.empty()) out = "{}";
  // Ensure app sees ok/available fields.
  if (out.back() == '}') {
    if (out.size() > 2) out.pop_back(), out += ",\"ok\":true,\"available\":true}";
    else out = "{\"ok\":true,\"available\":true}";
  }
  return out;
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
  if (access(kTailscaleCtl, X_OK) != 0) {
    return "{\"ok\":false,\"error\":\"tailscalectl missing\",\"available\":false}";
  }
  if (authkey.empty()) {
    return "{\"ok\":false,\"error\":\"auth key required\",\"available\":true}";
  }

  int rc = 0;
  std::string out;
  std::string err;
  if (!run_command({kTailscaleCtl, "set-auth-key", authkey, "--json"}, &rc, &out, &err)) {
    return "{\"ok\":false,\"available\":true,\"error\":\"spawn failed\"}";
  }

  out = trim_copy(out);
  if (rc != 0) {
    std::string msg = trim_copy(err.empty() ? out : err);
    if (msg.empty()) msg = "tailscalectl set-auth-key failed";
    return "{\"ok\":false,\"available\":true,\"error\":\"" + json_escape(msg) + "\"}";
  }

  if (out.empty()) out = "{}";
  if (out.back() == '}') {
    if (out.size() > 2) out.pop_back(), out += ",\"ok\":true,\"available\":true}";
    else out = "{\"ok\":true,\"available\":true}";
  }
  return out;
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

  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

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
      if (req.path == "/commaview/version") {
        return make_json(200, "{\"version\":\"0.1.3-alpha\"}");
      }
      if (req.path == "/commaview/status") {
        std::string body = "{\"version\":\"0.1.3-alpha\",\"api_port\":5002,\"tailscale\":" + tailscale_status() + "}";
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
