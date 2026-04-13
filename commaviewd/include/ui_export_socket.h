#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace commaview::ui_export {

inline constexpr uint8_t kFrameVersion = 1;
inline constexpr uint8_t kServiceCount = 18;
inline constexpr uint64_t kFreshFrameWindowMs = 750;

struct LatestFrame {
  bool available = false;
  uint8_t service_index = 0;
  uint64_t updated_at_ms = 0;
  std::vector<uint8_t> payload = {};
};

struct SocketStats {
  bool running = false;
  bool connected = false;
  uint64_t connect_count = 0;
  uint64_t accepted_count = 0;
  uint64_t malformed_count = 0;
  uint64_t last_receive_ms = 0;
};

std::string default_socket_path();

class SocketServer {
 public:
  explicit SocketServer(std::string socket_path = default_socket_path());
  ~SocketServer();

  bool start();
  void stop();

  bool latest_frame(uint8_t service_index, uint64_t fresh_within_ms, LatestFrame* out) const;
  SocketStats stats() const;

 private:
  void accept_loop();
  bool receive_one_frame(int client_fd);
  void mark_client_connected(bool connected);

  std::string socket_path_;
  std::atomic<bool> running_{false};
  int server_fd_ = -1;
  std::thread accept_thread_;

  mutable std::mutex mutex_;
  std::array<LatestFrame, kServiceCount> latest_ = {};
  SocketStats stats_ = {};
};

}  // namespace commaview::ui_export
