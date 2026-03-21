#pragma once

#include "telemetry_policy.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace commaview::runtime_debug {

inline constexpr const char* kRuntimeDebugConfigPath = "/data/commaview/config/runtime-debug.json";
inline constexpr const char* kRuntimeDebugDefaultsPath = "/data/commaview/runtime-debug.defaults.json";
inline constexpr const char* kRuntimeDebugEffectivePath = "/data/commaview/run/runtime-debug-effective.json";
inline constexpr const char* kRuntimeDebugStatsPath = "/data/commaview/run/telemetry-stats.json";
inline constexpr int kRuntimeDebugConfigVersion = 1;

struct LoadedRuntimeDebugConfig {
  int config_version = kRuntimeDebugConfigVersion;
  std::string instrumentation_level = "standard";
  std::unordered_map<std::string, telemetry::ServicePolicy> service_policies;
  bool exists = false;
  bool valid = true;
  bool safe_fallback = false;
  std::string source_path = kRuntimeDebugConfigPath;
  std::string error;
  std::vector<std::string> warnings;
  std::string config_hash;
};

inline std::string trim_copy(const std::string& in) {
  size_t s = 0;
  while (s < in.size() && std::isspace(static_cast<unsigned char>(in[s]))) s++;
  size_t e = in.size();
  while (e > s && std::isspace(static_cast<unsigned char>(in[e - 1]))) e--;
  return in.substr(s, e - s);
}

inline std::string json_escape(const std::string& in) {
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

inline std::string lower_copy(const std::string& in) {
  std::string out = in;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

inline bool read_text_file(const std::string& path, std::string* out) {
  if (out == nullptr) return false;
  std::ifstream f(path);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

inline std::string runtime_debug_config_path() {
  const char* path = std::getenv("COMMAVIEWD_RUNTIME_DEBUG_CONFIG");
  return (path != nullptr && path[0] != '\0') ? path : kRuntimeDebugConfigPath;
}

inline std::string runtime_debug_defaults_path() {
  const char* path = std::getenv("COMMAVIEWD_RUNTIME_DEBUG_DEFAULTS");
  return (path != nullptr && path[0] != '\0') ? path : kRuntimeDebugDefaultsPath;
}

inline std::string runtime_debug_effective_path() {
  const char* path = std::getenv("COMMAVIEWD_RUNTIME_DEBUG_EFFECTIVE");
  return (path != nullptr && path[0] != '\0') ? path : kRuntimeDebugEffectivePath;
}

inline std::string runtime_debug_stats_path() {
  const char* path = std::getenv("COMMAVIEWD_RUNTIME_STATS");
  return (path != nullptr && path[0] != '\0') ? path : kRuntimeDebugStatsPath;
}

inline std::string service_mode_to_string(telemetry::ServiceMode mode) {
  switch (mode) {
    case telemetry::ServiceMode::Off: return "off";
    case telemetry::ServiceMode::Sample: return "sample";
    case telemetry::ServiceMode::Pass: return "pass";
  }
  return "off";
}

inline bool parse_service_mode(const std::string& text, telemetry::ServiceMode* mode_out) {
  if (mode_out == nullptr) return false;
  const std::string lower = lower_copy(trim_copy(text));
  if (lower == "off") {
    *mode_out = telemetry::ServiceMode::Off;
    return true;
  }
  if (lower == "sample") {
    *mode_out = telemetry::ServiceMode::Sample;
    return true;
  }
  if (lower == "pass") {
    *mode_out = telemetry::ServiceMode::Pass;
    return true;
  }
  return false;
}

inline std::unordered_map<std::string, telemetry::ServicePolicy> default_service_policy_map() {
  std::unordered_map<std::string, telemetry::ServicePolicy> out;
  out.reserve(telemetry::kDefaultServicePolicyCount);
  for (size_t i = 0; i < telemetry::kDefaultServicePolicyCount; ++i) {
    out.emplace(telemetry::kDefaultServicePolicies[i].service,
                telemetry::kDefaultServicePolicies[i].policy);
  }
  return out;
}

inline LoadedRuntimeDebugConfig default_runtime_debug_config(bool exists = false) {
  LoadedRuntimeDebugConfig cfg;
  cfg.exists = exists;
  cfg.valid = true;
  cfg.safe_fallback = false;
  cfg.source_path = runtime_debug_config_path();
  cfg.service_policies = default_service_policy_map();
  return cfg;
}

inline telemetry::ServicePolicy policy_for_service(const LoadedRuntimeDebugConfig& cfg, const char* service_name) {
  if (service_name == nullptr) return {};
  auto it = cfg.service_policies.find(service_name);
  if (it != cfg.service_policies.end()) return it->second;
  return telemetry::default_service_policy_for_name(service_name);
}

inline bool extract_string_field(const std::string& body, const char* key, std::string* value_out) {
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
  std::string out;
  out.reserve(32);
  bool escaped = false;
  for (; pos < body.size(); ++pos) {
    char c = body[pos];
    if (escaped) {
      switch (c) {
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        default: out.push_back(c); break;
      }
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') {
      *value_out = out;
      return true;
    }
    out.push_back(c);
  }
  return false;
}

inline bool extract_int_field(const std::string& body, const char* key, int* value_out) {
  if (key == nullptr || value_out == nullptr) return false;
  const std::string needle = std::string("\"") + key + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos) return false;
  pos = body.find(':', pos + needle.size());
  if (pos == std::string::npos) return false;
  pos++;
  while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) pos++;
  if (pos >= body.size()) return false;
  size_t end = pos;
  if (body[end] == '-') end++;
  while (end < body.size() && std::isdigit(static_cast<unsigned char>(body[end]))) end++;
  if (end == pos || (end == pos + 1 && body[pos] == '-')) return false;
  try {
    *value_out = std::stoi(body.substr(pos, end - pos));
    return true;
  } catch (...) {
    return false;
  }
}

inline bool extract_service_object(const std::string& body, const char* service_name, std::string* object_out) {
  if (service_name == nullptr || object_out == nullptr) return false;
  const std::string needle = std::string("\"") + service_name + "\"";
  size_t pos = body.find(needle);
  if (pos == std::string::npos) return false;
  pos = body.find('{', pos + needle.size());
  if (pos == std::string::npos) return false;
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (size_t i = pos; i < body.size(); ++i) {
    char c = body[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
      continue;
    }
    if (c == '{') depth++;
    if (c == '}') {
      depth--;
      if (depth == 0) {
        *object_out = body.substr(pos, i - pos + 1);
        return true;
      }
    }
  }
  return false;
}

inline bool parse_service_policy_object(const std::string& object_json,
                                        const telemetry::ServicePolicy& base_policy,
                                        telemetry::ServicePolicy* policy_out,
                                        std::string* error_out) {
  if (policy_out == nullptr) return false;
  telemetry::ServicePolicy parsed = base_policy;

  std::string mode_text;
  if (!extract_string_field(object_json, "mode", &mode_text)) {
    if (error_out != nullptr) *error_out = "missing service mode";
    return false;
  }
  if (!parse_service_mode(mode_text, &parsed.mode)) {
    if (error_out != nullptr) *error_out = "invalid service mode: " + mode_text;
    return false;
  }

  int sample_hz = parsed.sample_hz;
  if (extract_int_field(object_json, "sampleHz", &sample_hz)) {
    parsed.sample_hz = sample_hz;
  }

  if (parsed.mode == telemetry::ServiceMode::Sample && parsed.sample_hz <= 0) {
    if (error_out != nullptr) *error_out = "sample mode requires sampleHz > 0";
    return false;
  }
  if (parsed.mode != telemetry::ServiceMode::Sample) {
    parsed.sample_hz = 0;
  }

  *policy_out = parsed;
  return true;
}

inline uint64_t fnv1a64(const std::string& text) {
  uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : text) {
    hash ^= static_cast<uint64_t>(c);
    hash *= 1099511628211ULL;
  }
  return hash;
}

inline std::string hex64(uint64_t value) {
  static const char* digits = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<size_t>(i)] = digits[value & 0xF];
    value >>= 4;
  }
  return out;
}

inline std::string canonical_config_contents_json(const LoadedRuntimeDebugConfig& cfg) {
  std::ostringstream out;
  out << "{"
      << "\"configVersion\":" << cfg.config_version << ","
      << "\"instrumentationLevel\":\"" << json_escape(cfg.instrumentation_level) << "\",";
  out << "\"services\":{";
  for (size_t i = 0; i < telemetry::kDefaultServicePolicyCount; ++i) {
    if (i > 0) out << ",";
    const char* service_name = telemetry::kDefaultServicePolicies[i].service;
    telemetry::ServicePolicy policy = policy_for_service(cfg, service_name);
    out << "\"" << service_name << "\":{";
    out << "\"mode\":\"" << service_mode_to_string(policy.mode) << "\"";
    if (policy.mode == telemetry::ServiceMode::Sample) {
      out << ",\"sampleHz\":" << policy.sample_hz;
    }
    out << "}";
  }
  out << "}}";
  return out.str();
}

inline void finalize_config_hash(LoadedRuntimeDebugConfig* cfg) {
  if (cfg == nullptr) return;
  cfg->config_hash = hex64(fnv1a64(canonical_config_contents_json(*cfg)));
}

inline std::string warnings_json(const std::vector<std::string>& warnings) {
  std::ostringstream out;
  out << "[";
  for (size_t i = 0; i < warnings.size(); ++i) {
    if (i > 0) out << ",";
    out << "\"" << json_escape(warnings[i]) << "\"";
  }
  out << "]";
  return out.str();
}

inline std::string render_config_json(const LoadedRuntimeDebugConfig& cfg, bool include_runtime_meta) {
  std::ostringstream out;
  out << canonical_config_contents_json(cfg);
  if (!include_runtime_meta) return out.str();

  std::string base = out.str();
  if (!base.empty() && base.back() == '}') base.pop_back();
  base += ",\"sourcePath\":\"" + json_escape(cfg.source_path) + "\"";
  base += ",\"exists\":" + std::string(cfg.exists ? "true" : "false");
  base += ",\"valid\":" + std::string(cfg.valid ? "true" : "false");
  base += ",\"safeFallback\":" + std::string(cfg.safe_fallback ? "true" : "false");
  base += ",\"configHash\":\"" + json_escape(cfg.config_hash) + "\"";
  base += ",\"warnings\":" + warnings_json(cfg.warnings);
  if (!cfg.error.empty()) {
    base += ",\"error\":\"" + json_escape(cfg.error) + "\"";
  }
  base += "}";
  return base;
}

inline LoadedRuntimeDebugConfig parse_runtime_debug_config_body(const std::string& body,
                                                                bool exists,
                                                                const std::string& source_path) {
  LoadedRuntimeDebugConfig parsed = default_runtime_debug_config(exists);
  parsed.source_path = source_path;
  parsed.exists = exists;

  const std::string trimmed = trim_copy(body);
  if (trimmed.empty()) {
    parsed.valid = false;
    parsed.safe_fallback = true;
    parsed.error = "empty runtime debug config";
    parsed.warnings.push_back("runtime debug config empty; using safe defaults");
    finalize_config_hash(&parsed);
    return parsed;
  }

  int version = parsed.config_version;
  if (extract_int_field(trimmed, "configVersion", &version)) {
    parsed.config_version = version;
  }

  std::string instrumentation_level;
  if (extract_string_field(trimmed, "instrumentationLevel", &instrumentation_level)) {
    parsed.instrumentation_level = instrumentation_level;
  }

  for (size_t i = 0; i < telemetry::kDefaultServicePolicyCount; ++i) {
    const char* service_name = telemetry::kDefaultServicePolicies[i].service;
    std::string service_object;
    if (!extract_service_object(trimmed, service_name, &service_object)) {
      continue;
    }

    telemetry::ServicePolicy parsed_policy = telemetry::kDefaultServicePolicies[i].policy;
    std::string error;
    if (!parse_service_policy_object(service_object,
                                     telemetry::kDefaultServicePolicies[i].policy,
                                     &parsed_policy,
                                     &error)) {
      parsed = default_runtime_debug_config(exists);
      parsed.source_path = source_path;
      parsed.exists = exists;
      parsed.valid = false;
      parsed.safe_fallback = true;
      parsed.error = std::string("service ") + service_name + ": " + error;
      parsed.warnings.push_back("invalid runtime debug config; using safe defaults");
      finalize_config_hash(&parsed);
      return parsed;
    }

    parsed.service_policies[service_name] = parsed_policy;
  }

  if (parsed.config_version <= 0) {
    parsed = default_runtime_debug_config(exists);
    parsed.source_path = source_path;
    parsed.exists = exists;
    parsed.valid = false;
    parsed.safe_fallback = true;
    parsed.error = "configVersion must be > 0";
    parsed.warnings.push_back("invalid runtime debug config; using safe defaults");
    finalize_config_hash(&parsed);
    return parsed;
  }

  parsed.valid = true;
  parsed.safe_fallback = false;
  finalize_config_hash(&parsed);
  return parsed;
}

inline LoadedRuntimeDebugConfig load_runtime_debug_config() {
  const std::string path = runtime_debug_config_path();
  std::string body;
  if (!read_text_file(path, &body)) {
    LoadedRuntimeDebugConfig cfg = default_runtime_debug_config(false);
    cfg.source_path = path;
    cfg.exists = false;
    cfg.valid = true;
    cfg.warnings.push_back("runtime debug config missing; using safe defaults");
    finalize_config_hash(&cfg);
    return cfg;
  }
  return parse_runtime_debug_config_body(body, true, path);
}

inline LoadedRuntimeDebugConfig effective_runtime_debug_config(const LoadedRuntimeDebugConfig& loaded) {
  LoadedRuntimeDebugConfig effective = loaded;
  if (!loaded.valid || loaded.safe_fallback) {
    effective = default_runtime_debug_config(true);
    effective.source_path = loaded.source_path;
    effective.exists = loaded.exists;
    effective.valid = true;
    effective.safe_fallback = true;
    effective.error.clear();
    effective.warnings = loaded.warnings;
  }
  finalize_config_hash(&effective);
  return effective;
}

inline std::string default_runtime_debug_config_json() {
  LoadedRuntimeDebugConfig cfg = default_runtime_debug_config(true);
  finalize_config_hash(&cfg);
  return canonical_config_contents_json(cfg);
}

}  // namespace commaview::runtime_debug
