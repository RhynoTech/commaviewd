#include "ui_export_socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace commaview::ui_export {
namespace {

constexpr uint32_t kMaxFrameBytes = 512 * 1024;
constexpr size_t kMaxRecipeQueue = 128;

uint64_t now_ms() {
  struct timespec ts = {};
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL + static_cast<uint64_t>(ts.tv_nsec / 1000000ULL);
}

bool recv_all_exact(int fd, uint8_t* data, size_t len) {
  size_t received = 0;
  while (received < len) {
    const ssize_t rc = recv(fd, data + received, len - received, 0);
    if (rc == 0) return false;
    if (rc < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    received += static_cast<size_t>(rc);
  }
  return true;
}

bool ensure_parent_dirs(const std::string& path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) return true;
  const std::string dir = path.substr(0, slash);
  std::string current;
  for (size_t i = 0; i < dir.size(); ++i) {
    current.push_back(dir[i]);
    if (dir[i] != '/' || current.size() == 1) continue;
    if (mkdir(current.c_str(), 0775) != 0 && errno != EEXIST) return false;
  }
  if (mkdir(dir.c_str(), 0775) != 0 && errno != EEXIST) return false;
  return true;
}

std::string current_route_id() {
  const char* override_path = std::getenv("COMMAVIEWD_CURRENT_ROUTE_FILE");
  const char* path = override_path && override_path[0] ?
      override_path : "/data/params/d/CurrentRoute";
  FILE* file = std::fopen(path, "r");
  if (file == nullptr) return {};
  char buffer[128] = {};
  const bool read = std::fgets(buffer, sizeof(buffer), file) != nullptr;
  std::fclose(file);
  if (!read) return {};
  std::string route(buffer);
  while (!route.empty() && std::isspace(static_cast<unsigned char>(route.back()))) {
    route.pop_back();
  }
  if (route.empty() || route.size() > 96 || route.find("--") == std::string::npos) return {};
  for (char c : route) {
    if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
        !(c >= '0' && c <= '9') && c != '-' && c != '_') return {};
  }
  return route;
}

std::string projection_camera(const std::string& json) {
  const size_t key = json.find("\"camera\"");
  if (key == std::string::npos) return {};
  size_t pos = json.find(':', key + 8);
  if (pos == std::string::npos) return {};
  ++pos;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
  if (pos >= json.size() || json[pos++] != '"') return {};
  const size_t end = json.find('"', pos);
  if (end == std::string::npos) return {};
  const std::string camera = json.substr(pos, end - pos);
  return camera == "road" || camera == "wideRoad" ? camera : std::string{};
}

uint64_t projection_uint(const std::string& json, const char* name) {
  const std::string key = std::string("\"") + name + "\"";
  const size_t found = json.find(key);
  if (found == std::string::npos) return 0;
  size_t pos = json.find(':', found + key.size());
  if (pos == std::string::npos) return 0;
  ++pos;
  while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
  if (pos >= json.size() || !std::isdigit(static_cast<unsigned char>(json[pos]))) return 0;
  uint64_t value = 0;
  while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
    const uint64_t digit = static_cast<unsigned char>(json[pos]) - '0';
    if (value > (UINT64_MAX - digit) / 10) return 0;
    value = value * 10 + digit;
    ++pos;
  }
  return value;
}

}  // namespace

std::string default_socket_path() {
  const char* env = std::getenv("COMMAVIEWD_UI_EXPORT_SOCKET");
  if (env != nullptr && env[0] != '\0') return env;
  return "/data/commaview/run/ui-export.sock";
}

std::string default_recipe_dir() {
  const char* env = std::getenv("COMMAVIEWD_RECIPE_DIR");
  if (env != nullptr && env[0] != '\0') return env;
  // Temporary bench marker is shared with the onroad UI exporter. A stale
  // marker cannot silently turn recording back on after a future reboot.
  const char* marker_env = std::getenv("COMMAVIEW_SOURCE_RECIPE_MARKER");
  const char* marker = marker_env && marker_env[0] ?
      marker_env : "/data/commaview/run/source-recipe-enabled";
  FILE* file = std::fopen(marker, "r");
  if (file == nullptr) return {};
  long long expires_at = 0;
  const bool valid = std::fscanf(file, "%lld", &expires_at) == 1;
  std::fclose(file);
  if (valid && expires_at > static_cast<long long>(std::time(nullptr))) {
    return "/data/commaview/recording-recipes";
  }
  return {};
}

SocketServer::SocketServer(std::string socket_path, std::string recipe_dir)
    : socket_path_(std::move(socket_path)), recipe_dir_(std::move(recipe_dir)) {}

SocketServer::~SocketServer() {
  stop();
}

bool SocketServer::start() {
  if (running_.load()) return true;
  if (!ensure_parent_dirs(socket_path_)) {
    std::fprintf(stderr, "[ui-export] failed to create parent dirs for %s\n", socket_path_.c_str());
    return false;
  }

  unlink(socket_path_.c_str());

  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    std::perror("socket(AF_UNIX)");
    return false;
  }

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    std::fprintf(stderr, "[ui-export] socket path too long: %s\n", socket_path_.c_str());
    close(fd);
    return false;
  }
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path_.c_str());

  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::perror("bind(AF_UNIX)");
    close(fd);
    return false;
  }

  if (listen(fd, 1) != 0) {
    std::perror("listen(AF_UNIX)");
    close(fd);
    unlink(socket_path_.c_str());
    return false;
  }

  server_fd_ = fd;
  if (!recipe_dir_.empty()) {
    recipe_file_path_ = recipe_dir_ + "/ui-source-" + std::to_string(now_ms()) +
                        "-" + std::to_string(getpid()) + ".jsonl";
    if (ensure_parent_dirs(recipe_file_path_)) {
      chmod(recipe_dir_.c_str(), 0700);
      recipe_file_ = std::fopen(recipe_file_path_.c_str(), "a");
      if (recipe_file_ != nullptr) fchmod(fileno(recipe_file_), 0600);
    }
    if (recipe_file_ == nullptr) {
      std::fprintf(stderr, "[ui-export] source recipe disabled: cannot open %s\n",
                   recipe_file_path_.c_str());
      recipe_file_path_.clear();
    } else {
      recipe_stopping_ = false;
      recipe_thread_ = std::thread(&SocketServer::recipe_writer_loop, this);
    }
  }
  running_.store(true);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.running = true;
  }
  accept_thread_ = std::thread(&SocketServer::accept_loop, this);
  return true;
}

void SocketServer::stop() {
  const bool was_running = running_.exchange(false);
  if (!was_running) return;

  if (server_fd_ >= 0) {
    close(server_fd_);
    server_fd_ = -1;
  }
  if (accept_thread_.joinable()) accept_thread_.join();
  {
    std::lock_guard<std::mutex> lock(recipe_mutex_);
    recipe_stopping_ = true;
  }
  recipe_condition_.notify_all();
  if (recipe_thread_.joinable()) recipe_thread_.join();
  {
    std::lock_guard<std::mutex> lock(recipe_mutex_);
    if (recipe_file_ != nullptr) {
      std::fclose(recipe_file_);
      recipe_file_ = nullptr;
    }
  }
  unlink(socket_path_.c_str());

  std::lock_guard<std::mutex> lock(mutex_);
  stats_.running = false;
  stats_.connected = false;
}

bool SocketServer::latest_frame(uint8_t service_index, uint64_t fresh_within_ms, LatestFrame* out) const {
  if (service_index >= kServiceCount || out == nullptr) return false;
  const uint64_t now = now_ms();
  std::lock_guard<std::mutex> lock(mutex_);
  const LatestFrame& frame = latest_[service_index];
  if (!frame.available) return false;
  if (fresh_within_ms > 0 && (now < frame.updated_at_ms || (now - frame.updated_at_ms) > fresh_within_ms)) {
    return false;
  }
  *out = frame;
  return true;
}

SocketStats SocketServer::stats() const {
  SocketStats result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    result = stats_;
  }
  {
    std::lock_guard<std::mutex> lock(recipe_mutex_);
    result.recipe_events = recipe_events_;
    result.recipe_dropped = recipe_dropped_;
    result.recipe_write_failures = recipe_write_failures_;
    result.recipe_missing_route = recipe_missing_route_;
    result.recipe_active = recipe_file_ != nullptr;
  }
  return result;
}

std::string SocketServer::recipe_file_path() const {
  std::lock_guard<std::mutex> lock(recipe_mutex_);
  return recipe_file_path_;
}

void SocketServer::offer_recipe_event(const LatestFrame& frame) {
  if (frame.service_index != kRecipeEventServiceIndex || recipe_file_ == nullptr ||
      frame.payload.size() > 4096) return;
  const std::string json(frame.payload.begin(), frame.payload.end());
  if (json.size() < 2 || json.front() != '{' || json.back() != '}' ||
      json.find_first_of("\r\n") != std::string::npos) return;
  const bool switched = json.find("\"kind\":\"camera_switch\"") != std::string::npos;
  const bool anchor = json.find("\"kind\":\"anchor\"") != std::string::npos;
  if (!switched && !anchor) return;
  if (projection_uint(json, "sequence") == 0) return;
  const std::string camera = projection_camera(json);
  if (camera.empty()) return;
  if (projection_uint(json, "logMonoTime") == 0 ||
      projection_uint(json, camera == "road" ? "roadTimestampEof" : "wideTimestampEof") == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(recipe_mutex_);
  if (recipe_queue_.size() >= kMaxRecipeQueue) {
    ++recipe_dropped_;
    return;
  }
  recipe_queue_.push_back("{\"schemaVersion\":1,\"capturedWallMs\":" +
                          std::to_string(frame.updated_at_ms) + "," + json.substr(1) + "\n");
  recipe_condition_.notify_one();
}

void SocketServer::recipe_writer_loop() {
  while (true) {
    std::string line;
    {
      std::unique_lock<std::mutex> lock(recipe_mutex_);
      recipe_condition_.wait(lock, [this] { return recipe_stopping_ || !recipe_queue_.empty(); });
      if (recipe_queue_.empty() && recipe_stopping_) break;
      line = std::move(recipe_queue_.front());
      recipe_queue_.pop_front();
    }
    const std::string route = current_route_id();
    if (route.empty()) {
      line.insert(1, "\"routeId\":null,");
    } else {
      line.insert(1, "\"routeId\":\"" + route + "\",");
    }
    const bool written = std::fwrite(line.data(), 1, line.size(), recipe_file_) == line.size() &&
                         std::fflush(recipe_file_) == 0 &&
                         fdatasync(fileno(recipe_file_)) == 0;
    std::lock_guard<std::mutex> lock(recipe_mutex_);
    if (route.empty()) ++recipe_missing_route_;
    if (written) ++recipe_events_;
    else ++recipe_write_failures_;
  }
}

void SocketServer::mark_client_connected(bool connected) {
  std::lock_guard<std::mutex> lock(mutex_);
  stats_.connected = connected;
  if (connected) stats_.connect_count += 1;
}

void SocketServer::accept_loop() {
  while (running_.load()) {
    const int client_fd = accept(server_fd_, nullptr, nullptr);
    if (client_fd < 0) {
      if (running_.load() && errno != EINTR) std::perror("accept(AF_UNIX)");
      continue;
    }

    mark_client_connected(true);
    while (running_.load() && receive_one_frame(client_fd)) {
    }
    close(client_fd);
    mark_client_connected(false);
  }
}

bool SocketServer::receive_one_frame(int client_fd) {
  uint8_t len_buf[4] = {};
  if (!recv_all_exact(client_fd, len_buf, sizeof(len_buf))) return false;

  const uint32_t payload_len = ntohl(*reinterpret_cast<uint32_t*>(len_buf));
  if (payload_len < 3 || payload_len > kMaxFrameBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.malformed_count += 1;
    return false;
  }

  std::vector<uint8_t> payload(payload_len);
  if (!recv_all_exact(client_fd, payload.data(), payload.size())) return false;

  const uint8_t version = payload[0];
  const uint8_t service_index = payload[1];
  if (version != kFrameVersion || service_index > kRecipeEventServiceIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.malformed_count += 1;
    return false;
  }

  LatestFrame frame;
  frame.available = true;
  frame.service_index = service_index;
  frame.updated_at_ms = now_ms();
  frame.payload.assign(payload.begin() + 2, payload.end());

  if (service_index == kRecipeEventServiceIndex) {
    offer_recipe_event(frame);
    return true;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_[service_index] = frame;
    stats_.accepted_count += 1;
    stats_.last_receive_ms = frame.updated_at_ms;
  }
  return true;
}

}  // namespace commaview::ui_export
