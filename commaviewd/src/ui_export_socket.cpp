#include "ui_export_socket.h"

#include <arpa/inet.h>
#include <cerrno>
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

}  // namespace

std::string default_socket_path() {
  const char* env = std::getenv("COMMAVIEWD_UI_EXPORT_SOCKET");
  if (env != nullptr && env[0] != '\0') return env;
  return "/data/commaview/run/ui-export.sock";
}

SocketServer::SocketServer(std::string socket_path)
    : socket_path_(std::move(socket_path)) {}

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
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
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
  if (version != kFrameVersion || service_index >= kServiceCount) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.malformed_count += 1;
    return false;
  }

  LatestFrame frame;
  frame.available = true;
  frame.service_index = service_index;
  frame.updated_at_ms = now_ms();
  frame.payload.assign(payload.begin() + 2, payload.end());

  std::lock_guard<std::mutex> lock(mutex_);
  latest_[service_index] = frame;
  stats_.accepted_count += 1;
  stats_.last_receive_ms = frame.updated_at_ms;
  return true;
}

}  // namespace commaview::ui_export
