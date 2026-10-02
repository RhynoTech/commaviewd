#include "ui_export_socket.h"

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
  assert(commaview::ui_export::default_recipe_dir() == "/data/commaview/recording-recipes");
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

  // UI can wake before loggerd writes CurrentRoute. Those initial events
  // must not poison route-bound preflight for the rest of the drive.
  assert(send_frame(client_fd, commaview::ui_export::kRecipeEventServiceIndex,
                    R"({"sequence":1,"kind":"camera_switch","projection":{"camera":"road","logMonoTime":90,"roadTimestampEof":80}})"));
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  assert(server.stats().recipe_events == 0);
  assert(server.stats().recipe_missing_route == 0);
  {
    std::ofstream route(route_file);
    route << "000004b4--75e1f0ba8f\n";
  }
  assert(send_frame(client_fd, 0, R"({"exportVersion":4,"speedMps":12.4,"logMonoTime":457})"));

  const auto stats = server.stats();
  assert(stats.running);
  assert(stats.connect_count >= 1);
  assert(stats.accepted_count >= 1);

  assert(send_frame(client_fd, 18, R"({"camera":"road","logMonoTime":100,"roadFrameId":1,"roadTimestampEof":90})"));
  for (int i = 0; i < 500; ++i) {
    assert(send_frame(client_fd, 18, R"({"camera":"road","logMonoTime":101,"roadFrameId":2,"roadTimestampEof":91})"));
  }
  assert(send_frame(client_fd, 18, R"({"camera":"wideRoad","logMonoTime":102,"wideFrameId":3,"wideTimestampEof":92})"));
  assert(send_frame(client_fd, 2, R"({"speed":12.4,"logMonoTime":103})"));
  assert(send_frame(client_fd, 7, R"({"frameId":4,"logMonoTime":104})"));
  // The live projection path is conflated; only explicit source UI events form
  // the recording timeline, even when camera changes arrive faster than 20 Hz.
  assert(send_frame(client_fd, commaview::ui_export::kRecipeEventServiceIndex,
                    R"({"sequence":2,"kind":"camera_switch","projection":{"camera":"road","logMonoTime":100,"roadTimestampEof":90}})"));
  assert(send_frame(client_fd, commaview::ui_export::kRecipeEventServiceIndex,
                    R"({"sequence":3,"kind":"camera_switch","projection":{"camera":"wideRoad","logMonoTime":102,"wideTimestampEof":92}})"));
  bool got_recipe = false;
  for (int i = 0; i < 100; ++i) {
    const auto current = server.stats();
    if (current.recipe_events == 2) {
      assert(current.recipe_route == "000004b4--75e1f0ba8f");
      assert(current.recipe_dropped == 0);
      assert(current.recipe_write_failures == 0);
      assert(current.recipe_missing_route == 0);
      got_recipe = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(got_recipe);
  bool got_snapshots = false;
  for (int i = 0; i < 100; ++i) {
    const auto current = server.stats();
    if (current.snapshot_events >= 4) {
      assert(current.snapshot_active);
      assert(current.snapshot_route == "000004b4--75e1f0ba8f");
      assert(current.snapshot_dropped == 0);
      assert(current.snapshot_write_failures == 0);
      assert(current.snapshot_events <= 6);  // 500 fast projection offers were sampled.
      got_snapshots = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(got_snapshots);
  const std::string recipe_path = server.recipe_file_path();

  // Offroad, CurrentRoute stays set and the comma's UI keeps exporting; the finished route's
  // snapshot must stop growing so the app can download it whole.
  const std::string offroad_param = std::string(dir) + "/IsOffroad";
  {
    std::ofstream offroad(offroad_param);
    offroad << "1";
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));  // Past the writer's 1 s re-check.
  bool snapshot_closed = false;
  for (int i = 0; i < 100 && !snapshot_closed; ++i) {
    assert(send_frame(client_fd, 2, R"({"speed":0.0,"logMonoTime":200})"));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    snapshot_closed = !server.stats().snapshot_active;
  }
  assert(snapshot_closed);
  const auto closed = server.stats();
  const std::string offroad_snapshot_path = recipe_dir + "/ui-snapshot-000004b4--75e1f0ba8f.jsonl";
  std::ifstream before_file(offroad_snapshot_path, std::ios::ate | std::ios::binary);
  const auto size_before = before_file.tellg();
  for (int i = 0; i < 5; ++i) {
    assert(send_frame(client_fd, 2, R"({"speed":0.0,"logMonoTime":300})"));
    assert(send_frame(client_fd, 7, R"({"frameId":9,"logMonoTime":301})"));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  }
  std::ifstream after_file(offroad_snapshot_path, std::ios::ate | std::ios::binary);
  assert(after_file.tellg() == size_before);
  assert(server.stats().snapshot_events == closed.snapshot_events);
  unlink(offroad_param.c_str());

  // Shutdown must also wake a still-connected UI producer blocked in recv().
  server.stop();
  close(client_fd);
  std::ifstream recipe(recipe_path);
  std::string first;
  std::string second;
  std::string third;
  assert(static_cast<bool>(std::getline(recipe, first)));
  assert(static_cast<bool>(std::getline(recipe, second)));
  assert(!static_cast<bool>(std::getline(recipe, third)));
  assert(first.find("\"camera\":\"road\"") != std::string::npos);
  assert(second.find("\"camera\":\"wideRoad\"") != std::string::npos);
  assert(first.find("\"sequence\":2") != std::string::npos);
  assert(second.find("\"sequence\":3") != std::string::npos);
  assert(first.find("\"routeId\":\"000004b4--75e1f0ba8f\"") != std::string::npos);
  assert(second.find("\"routeId\":\"000004b4--75e1f0ba8f\"") != std::string::npos);
  const std::string snapshot_path = recipe_dir + "/ui-snapshot-000004b4--75e1f0ba8f.jsonl";
  std::ifstream snapshots(snapshot_path);
  std::string snapshot_line;
  bool has_ui = false;
  bool has_projection = false;
  bool has_car = false;
  bool has_model = false;
  uint64_t prior_snapshot_sequence = 0;
  while (std::getline(snapshots, snapshot_line)) {
    const size_t sequence_at = snapshot_line.find("\"sequence\":");
    assert(sequence_at != std::string::npos);
    const uint64_t sequence = std::stoull(snapshot_line.substr(sequence_at + 11));
    if (prior_snapshot_sequence) assert(sequence == prior_snapshot_sequence + 1);
    prior_snapshot_sequence = sequence;
    has_ui |= snapshot_line.find("\"serviceIndex\":0") != std::string::npos;
    has_projection |= snapshot_line.find("\"serviceIndex\":18") != std::string::npos;
    has_car |= snapshot_line.find("\"serviceIndex\":2") != std::string::npos;
    has_model |= snapshot_line.find("\"serviceIndex\":7") != std::string::npos;
  }
  assert(has_ui && has_projection && has_car && has_model);
  unsetenv("COMMAVIEWD_CURRENT_ROUTE_FILE");
  unlink(route_file.c_str());
  unlink(recipe_path.c_str());
  unlink(snapshot_path.c_str());
  rmdir(recipe_dir.c_str());
  unlink(socket_path.c_str());
  rmdir(dir);
  return 0;
}
