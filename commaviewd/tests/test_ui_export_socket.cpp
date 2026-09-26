#include "ui_export_socket.h"

#include <arpa/inet.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <fstream>
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
  const std::string route_file = std::string(dir) + "/CurrentRoute";
  const std::string marker_file = std::string(dir) + "/source-recipe-enabled";
  assert(setenv("COMMAVIEW_SOURCE_RECIPE_MARKER", marker_file.c_str(), 1) == 0);
  {
    std::ofstream marker(marker_file);
    marker << static_cast<long long>(std::time(nullptr)) + 3600 << "\n";
  }
  assert(commaview::ui_export::default_recipe_dir() == "/data/commaview/recording-recipes");
  {
    std::ofstream marker(marker_file);
    marker << static_cast<long long>(std::time(nullptr)) - 1 << "\n";
  }
  assert(commaview::ui_export::default_recipe_dir().empty());
  unsetenv("COMMAVIEW_SOURCE_RECIPE_MARKER");
  unlink(marker_file.c_str());
  {
    std::ofstream route(route_file);
    route << "000004b4--75e1f0ba8f\n";
  }
  assert(setenv("COMMAVIEWD_CURRENT_ROUTE_FILE", route_file.c_str(), 1) == 0);

  const std::string recipe_dir = std::string(dir) + "/recipes";
  commaview::ui_export::SocketServer server(socket_path, recipe_dir);
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

  assert(send_frame(client_fd, 18, R"({"camera":"road","logMonoTime":100,"roadFrameId":1,"roadTimestampEof":90})"));
  for (int i = 0; i < 500; ++i) {
    assert(send_frame(client_fd, 18, R"({"camera":"road","logMonoTime":101,"roadFrameId":2,"roadTimestampEof":91})"));
  }
  assert(send_frame(client_fd, 18, R"({"camera":"wideRoad","logMonoTime":102,"wideFrameId":3,"wideTimestampEof":92})"));
  // The live projection path is conflated; only explicit source UI events form
  // the recording timeline, even when camera changes arrive faster than 20 Hz.
  assert(send_frame(client_fd, commaview::ui_export::kRecipeEventServiceIndex,
                    R"({"sequence":1,"kind":"camera_switch","projection":{"camera":"road","logMonoTime":100,"roadTimestampEof":90}})"));
  assert(send_frame(client_fd, commaview::ui_export::kRecipeEventServiceIndex,
                    R"({"sequence":2,"kind":"camera_switch","projection":{"camera":"wideRoad","logMonoTime":102,"wideTimestampEof":92}})"));
  bool got_recipe = false;
  for (int i = 0; i < 100; ++i) {
    const auto current = server.stats();
    if (current.recipe_events == 2) {
      assert(current.recipe_dropped == 0);
      assert(current.recipe_write_failures == 0);
      assert(current.recipe_missing_route == 0);
      got_recipe = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(got_recipe);
  const std::string recipe_path = server.recipe_file_path();

  close(client_fd);
  server.stop();
  std::ifstream recipe(recipe_path);
  std::string first;
  std::string second;
  std::string third;
  assert(static_cast<bool>(std::getline(recipe, first)));
  assert(static_cast<bool>(std::getline(recipe, second)));
  assert(!static_cast<bool>(std::getline(recipe, third)));
  assert(first.find("\"camera\":\"road\"") != std::string::npos);
  assert(second.find("\"camera\":\"wideRoad\"") != std::string::npos);
  assert(first.find("\"sequence\":1") != std::string::npos);
  assert(second.find("\"sequence\":2") != std::string::npos);
  assert(first.find("\"routeId\":\"000004b4--75e1f0ba8f\"") != std::string::npos);
  assert(second.find("\"routeId\":\"000004b4--75e1f0ba8f\"") != std::string::npos);
  unsetenv("COMMAVIEWD_CURRENT_ROUTE_FILE");
  unlink(route_file.c_str());
  unlink(recipe_path.c_str());
  rmdir(recipe_dir.c_str());
  unlink(socket_path.c_str());
  rmdir(dir);
  return 0;
}
