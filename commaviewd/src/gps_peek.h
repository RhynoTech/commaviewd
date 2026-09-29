#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace commaview::gps {

// A position from the comma's own GPS.
struct Fix {
  double lat = 0.0;
  double lon = 0.0;
  float accuracy_m = 0.0f;
  float bearing_deg = 0.0f;
  float speed_ms = 0.0f;
  int64_t fix_ms = 0;       // GPS time
  uint64_t log_mono_ns = 0;  // when openpilot published it
};

// The fix in one serialized cereal Event, or nothing when it isn't a GPS event with a usable fix.
std::optional<Fix> fix_from_event(const uint8_t* data, size_t size);

// The newest message in one of openpilot's msgq queues, read without subscribing. It maps the
// queue's shared memory read-only and never writes to it (no reader slot, no read pointer, no
// signal), so openpilot's publisher and subscribers can't tell it's there. It walks the current
// lap of the ring from its start to the write pointer and copies only the last message; a read the
// writer may have overtaken is dropped rather than returned.
class QueuePeek {
 public:
  explicit QueuePeek(std::string path);
  ~QueuePeek();
  QueuePeek(const QueuePeek&) = delete;
  QueuePeek& operator=(const QueuePeek&) = delete;

  std::optional<std::vector<uint8_t>> newest();

 private:
  bool ensure_mapped();
  void unmap();

  std::string path_;
  const char* mem_ = nullptr;
  size_t map_size_ = 0;
  size_t data_size_ = 0;
  ino_t inode_ = 0;
};

// The newest fix from openpilot's GPS queues (u-blox and Qualcomm), read without subscribing, and
// kept between calls so a moment without a fix still has the last one.
class GpsPeek {
 public:
  explicit GpsPeek(const std::string& msgq_dir = "/dev/shm");
  std::optional<Fix> latest();

 private:
  QueuePeek external_;
  QueuePeek qualcomm_;
  std::optional<Fix> latest_;
};

}  // namespace commaview::gps
