#include "ui_export_socket.h"

#include <arpa/inet.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

bool connect_unix_socket(const std::string& path, int* out_fd) {
  if (out_fd == nullptr) return false;
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;

  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return false;
  }
  *out_fd = fd;
  return true;
}

bool send_frame(int fd, uint8_t service_index, const std::string& json) {
  std::string payload;
  payload.push_back(static_cast<char>(commaview::ui_export::kFrameVersion));
  payload.push_back(static_cast<char>(service_index));
  payload += json;

  const uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
  if (send(fd, &len, sizeof(len), 0) != static_cast<ssize_t>(sizeof(len))) return false;
  return send(fd, payload.data(), payload.size(), 0) == static_cast<ssize_t>(payload.size());
}

}  // namespace

int main() {
  char path_template[] = "/tmp/commaview-ui-export-XXXXXX";
  char* dir = mkdtemp(path_template);
  assert(dir != nullptr);
  const std::string socket_path = std::string(dir) + "/ui-export.sock";

  commaview::ui_export::SocketServer server(socket_path);
  assert(server.start());

  int client_fd = -1;
  for (int i = 0; i < 50 && client_fd < 0; ++i) {
    if (connect_unix_socket(socket_path, &client_fd)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(client_fd >= 0);
  assert(send_frame(client_fd, 0, R"({"exportVersion":4,"speedMps":12.3,"logMonoTime":456})"));

  commaview::ui_export::LatestFrame frame;
  bool got_frame = false;
  for (int i = 0; i < 50; ++i) {
    if (server.latest_frame(0, 1000, &frame)) {
      got_frame = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  assert(got_frame);
  assert(frame.available);
  assert(frame.service_index == 0);
  assert(std::string(frame.payload.begin(), frame.payload.end()).find("\"speedMps\":12.3") != std::string::npos);

  const auto stats = server.stats();
  assert(stats.running);
  assert(stats.connect_count >= 1);
  assert(stats.accepted_count >= 1);

  close(client_fd);
  server.stop();
  unlink(socket_path.c_str());
  rmdir(dir);
  return 0;
}
