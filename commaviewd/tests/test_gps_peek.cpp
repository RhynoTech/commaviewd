// The comma's GPS read straight from openpilot's msgq ring, without subscribing: newest fix, a
// wrapped ring, garbage and other events, and a header left exactly as it was.
#include "gps_peek.h"

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cereal/gen/cpp/log.capnp.h"
#include "msgq/msgq.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    failures++;
  }
}

// A queue file laid out as msgq lays it out, written the way msgq_msg_send writes it.
struct FakeQueue {
  std::string path;
  size_t size;
  uint32_t lap = 0;
  uint64_t offset = 0;

  FakeQueue(std::string p, size_t data_size) : path(std::move(p)), size(data_size) {
    std::vector<char> zeros(sizeof(msgq_header_t) + size, 0);
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(zeros.data(), 1, zeros.size(), f);
    std::fclose(f);
  }

  void write_at(uint64_t pos, const void* bytes, size_t n) {
    FILE* f = std::fopen(path.c_str(), "r+b");
    std::fseek(f, static_cast<long>(pos), SEEK_SET);
    std::fwrite(bytes, 1, n, f);
    std::fclose(f);
  }

  void publish(const std::vector<uint8_t>& message) {
    const uint64_t total = (message.size() + sizeof(int64_t) + 7) & ~uint64_t{7};
    const int64_t remaining = static_cast<int64_t>(size) - static_cast<int64_t>(offset) - static_cast<int64_t>(total) - 8;
    if (remaining <= 0) {
      const int64_t wrap = -1;
      write_at(sizeof(msgq_header_t) + offset, &wrap, sizeof(wrap));
      offset = 0;
      lap++;
    }
    const int64_t n = static_cast<int64_t>(message.size());
    write_at(sizeof(msgq_header_t) + offset, &n, sizeof(n));
    write_at(sizeof(msgq_header_t) + offset + sizeof(int64_t), message.data(), message.size());
    offset = (offset + sizeof(int64_t) + message.size() + 7) & ~uint64_t{7};
    const uint64_t pointer = (uint64_t{lap} << 32) | offset;
    write_at(offsetof(msgq_header_t, write_pointer), &pointer, sizeof(pointer));
  }

  std::vector<char> header() const {
    std::vector<char> out(sizeof(msgq_header_t));
    FILE* f = std::fopen(path.c_str(), "rb");
    size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    assert(got == out.size());
    return out;
  }
};

std::vector<uint8_t> gps_event(double lat, double lon, int64_t fix_ms, bool has_fix = true, bool qualcomm = false) {
  capnp::MallocMessageBuilder builder;
  auto event = builder.initRoot<cereal::Event>();
  event.setLogMonoTime(static_cast<uint64_t>(fix_ms) * 1000000ULL);
  auto gps = qualcomm ? event.initGpsLocation() : event.initGpsLocationExternal();
  gps.setLatitude(lat);
  gps.setLongitude(lon);
  gps.setHasFix(has_fix);
  gps.setHorizontalAccuracy(4.5f);
  gps.setBearingDeg(90.0f);
  gps.setSpeed(12.5f);
  gps.setUnixTimestampMillis(fix_ms);
  auto words = capnp::messageToFlatArray(builder);
  auto bytes = words.asBytes();
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

std::vector<uint8_t> car_event() {
  capnp::MallocMessageBuilder builder;
  auto event = builder.initRoot<cereal::Event>();
  event.initCarState().setVEgo(20.0f);
  auto words = capnp::messageToFlatArray(builder);
  auto bytes = words.asBytes();
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

}  // namespace

int main() {
  char dir_template[] = "/tmp/commaview-gps-peek-XXXXXX";
  const std::string dir = mkdtemp(dir_template);
  using commaview::gps::GpsPeek;
  using commaview::gps::QueuePeek;

  {
    GpsPeek peek(dir);
    check(!peek.latest().has_value(), "no queue, no fix");
  }

  FakeQueue ublox(dir + "/msgq_gpsLocationExternal", 1 << 20);
  GpsPeek peek(dir);
  check(!peek.latest().has_value(), "an empty queue has no fix");

  ublox.publish(gps_event(37.70, -122.40, 1780000000000));
  ublox.publish(gps_event(37.71, -122.41, 1780000001000));
  ublox.publish(gps_event(37.72, -122.42, 1780000002000));
  const auto before = ublox.header();
  auto fix = peek.latest();
  check(fix.has_value() && fix->lat == 37.72 && fix->lon == -122.42, "the newest fix is read");
  check(fix.has_value() && fix->fix_ms == 1780000002000 && fix->accuracy_m == 4.5f && fix->speed_ms == 12.5f,
        "the fix keeps its time, accuracy and speed");
  check(ublox.header() == before, "the queue's header is untouched: no reader slot, no read pointer");

  ublox.publish(gps_event(37.73, -122.43, 1780000003000, /*has_fix=*/false));
  fix = peek.latest();
  check(fix.has_value() && fix->lat == 37.72, "a moment without a fix keeps the last one");

  ublox.publish(car_event());
  auto other = QueuePeek(ublox.path).newest();
  check(other.has_value() && !commaview::gps::fix_from_event(other->data(), other->size()).has_value(),
        "another kind of event is not a fix");

  // A small ring that wraps: the newest is found in the new lap.
  FakeQueue small(dir + "/msgq_gpsLocation", 4096);
  QueuePeek small_peek(small.path);
  for (int i = 0; i < 40; i++) small.publish(gps_event(10.0 + i, 20.0, 1780000100000 + i, true, true));
  check(small.lap > 0, "the small ring wrapped");
  auto newest = small_peek.newest();
  auto newest_fix = newest ? commaview::gps::fix_from_event(newest->data(), newest->size()) : std::nullopt;
  check(newest_fix.has_value() && newest_fix->lat == 49.0, "after wrapping, the newest message is the last one written");

  // The Qualcomm queue counts too, and the later fix wins across the two.
  small.publish(gps_event(51.5, -0.12, 1780000009000, true, true));
  fix = peek.latest();
  check(fix.has_value() && fix->lat == 51.5 && fix->fix_ms == 1780000009000, "the newer fix of the two queues wins");

  // Garbage where a size should be: nothing, and no crash.
  FakeQueue broken(dir + "/broken", 4096);
  broken.publish(gps_event(1.0, 2.0, 1780000200000));
  const int64_t garbage = int64_t{1} << 40;
  broken.write_at(sizeof(msgq_header_t), &garbage, sizeof(garbage));
  check(!QueuePeek(broken.path).newest().has_value(), "a corrupt size is rejected");

  // Not a message at all.
  std::vector<uint8_t> junk(64, 0xAB);
  check(!commaview::gps::fix_from_event(junk.data(), junk.size()).has_value(), "junk bytes are no fix");
  check(!commaview::gps::fix_from_event(junk.data(), 7).has_value(), "a torn message is no fix");
  auto no_place = gps_event(0.0, 0.0, 1780000300000);
  check(!commaview::gps::fix_from_event(no_place.data(), no_place.size()).has_value(), "0,0 is no fix");

  std::string cleanup = "rm -rf '" + dir + "'";
  if (std::system(cleanup.c_str()) != 0) std::fprintf(stderr, "warning: could not remove %s\n", dir.c_str());
  if (failures == 0) std::printf("PASS: GPS read from msgq without subscribing\n");
  return failures == 0 ? 0 : 1;
}
