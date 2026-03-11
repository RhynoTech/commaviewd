#include "commaview/control/policy.h"

#include "commaview/net/framing.h"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <unordered_map>

namespace commaview::control {

namespace {

constexpr uint32_t MAX_INBOUND_FRAME_BYTES = 64 * 1024;

std::mutex g_session_policy_mu;
std::unordered_map<std::string, bool> g_session_policy;

bool extract_json_string_field(const std::string& json,
                               const char* key,
                               std::string* out) {
  if (out == nullptr || key == nullptr) return false;

  const std::string needle = std::string("\"") + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return false;

  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  pos++;

  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
  if (pos >= json.size() || json[pos] != '"') return false;
  pos++;

  size_t end = json.find('"', pos);
  if (end == std::string::npos) return false;

  *out = json.substr(pos, end - pos);
  return !out->empty();
}

bool extract_json_bool_field(const std::string& json,
                             const char* key,
                             bool* out) {
  if (out == nullptr || key == nullptr) return false;

  const std::string needle = std::string("\"") + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) return false;

  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  pos++;

  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) pos++;
  if (pos >= json.size()) return false;

  if (json.compare(pos, 4, "true") == 0) {
    *out = true;
    return true;
  }
  if (json.compare(pos, 5, "false") == 0) {
    *out = false;
    return true;
  }
  return false;
}

bool parse_set_policy_control(const std::string& json,
                              std::string* session_id,
                              bool* suppress_video) {
  std::string op;
  if (!extract_json_string_field(json, "op", &op) || op != "set_policy") return false;

  std::string sid;
  bool suppress = false;
  if (!extract_json_string_field(json, "sessionId", &sid)) return false;
  if (!extract_json_bool_field(json, "suppressVideo", &suppress)) return false;

  if (session_id != nullptr) *session_id = sid;
  if (suppress_video != nullptr) *suppress_video = suppress;
  return true;
}

}  // namespace


TailscalePolicyAction decide_tailscale_action(bool onroad, bool desired_enabled) {
  if (onroad) return TailscalePolicyAction::kForceDown;
  return desired_enabled ? TailscalePolicyAction::kEnsureUp : TailscalePolicyAction::kStayDown;
}

void set_session_policy(const std::string& session_id, bool suppress_video) {
  std::lock_guard<std::mutex> lock(g_session_policy_mu);
  g_session_policy[session_id] = suppress_video;
}

bool get_session_policy(const std::string& session_id, bool* suppress_video) {
  if (session_id.empty() || suppress_video == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_session_policy_mu);
  auto it = g_session_policy.find(session_id);
  if (it == g_session_policy.end()) return false;
  *suppress_video = it->second;
  return true;
}

void consume_client_control_frames(int client_fd,
                                   ClientControlState* state,
                                   const char* video_service,
                                   uint8_t msg_control_type) {
  if (state == nullptr) return;

  uint8_t tmp[4096];
  while (true) {
    ssize_t n = ::recv(client_fd, tmp, sizeof(tmp), MSG_DONTWAIT);
    if (n > 0) {
      state->rx_buf.insert(state->rx_buf.end(), tmp, tmp + n);
      continue;
    }
    if (n == 0) {
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    return;
  }

  while (state->rx_buf.size() >= 4) {
    uint32_t frame_len = commaview::net::read_be32(state->rx_buf.data());
    if (frame_len == 0 || frame_len > MAX_INBOUND_FRAME_BYTES) {
      state->parse_error_count++;
      if (state->parse_error_count <= 3 || (state->parse_error_count % 20) == 0) {
        printf("[%s] invalid inbound frame len=%u; dropping buffered input\n",
               video_service,
               frame_len);
        fflush(stdout);
      }
      state->rx_buf.clear();
      return;
    }

    const size_t total = static_cast<size_t>(4) + frame_len;
    if (state->rx_buf.size() < total) break;

    const uint8_t* payload = state->rx_buf.data() + 4;
    const uint8_t msg_type = payload[0];

    if (msg_type == msg_control_type) {
      const std::string json(reinterpret_cast<const char*>(payload + 1),
                             frame_len > 1 ? frame_len - 1 : 0);
      std::string session_id;
      bool suppress_video = false;
      if (parse_set_policy_control(json, &session_id, &suppress_video)) {
        state->bound_session_id = session_id;
        set_session_policy(session_id, suppress_video);
        state->control_update_count++;
        if (state->control_update_count <= 3 || (state->control_update_count % 100) == 0) {
          printf("[%s] control update session=%s suppress=%s\n",
                 video_service,
                 session_id.c_str(),
                 suppress_video ? "true" : "false");
          fflush(stdout);
        }
      } else {
        state->parse_error_count++;
        if (state->parse_error_count <= 3 || (state->parse_error_count % 20) == 0) {
          printf("[%s] invalid control payload ignored\n", video_service);
          fflush(stdout);
        }
      }
    }

    state->rx_buf.erase(state->rx_buf.begin(), state->rx_buf.begin() + total);
  }
}

}  // namespace commaview::control
