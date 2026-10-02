#include "gps_peek.h"

#include <capnp/serialize.h>
#include <kj/array.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <utility>

#include "cereal/gen/cpp/log.capnp.h"

namespace commaview::gps {
namespace {

// The writer may be partway through a message it hasn't published yet; stay this far from it.
constexpr uint64_t kInFlightMarginBytes = 64 * 1024;
// A GPS event is a few hundred bytes; a size beyond this isn't one.
constexpr int64_t kMaxMessageBytes = 64 * 1024;
constexpr size_t kMaxWalkSteps = size_t{1} << 20;

int64_t load_i64(const char* p) {
  return __atomic_load_n(reinterpret_cast<const int64_t*>(p), __ATOMIC_ACQUIRE);
}

uint64_t align8(uint64_t n) {
  return (n + 7) & ~uint64_t{7};
}

}  // namespace

std::optional<Fix> fix_from_event(const uint8_t* data, size_t size) {
  if (data == nullptr || size < sizeof(capnp::word) || size % sizeof(capnp::word) != 0) return std::nullopt;
  kj::Array<capnp::word> words = kj::heapArray<capnp::word>(size / sizeof(capnp::word));
  std::memcpy(words.begin(), data, size);
  try {
    capnp::ReaderOptions options;
    options.traversalLimitInWords = 1 << 16;
    capnp::FlatArrayMessageReader reader(words, options);
    auto event = reader.getRoot<cereal::Event>();
    cereal::GpsLocationData::Reader gps;
    if (event.isGpsLocationExternal()) {
      gps = event.getGpsLocationExternal();
    } else if (event.isGpsLocation()) {
      gps = event.getGpsLocation();
    } else {
      return std::nullopt;
    }
    const double lat = gps.getLatitude();
    const double lon = gps.getLongitude();
    if (!gps.getHasFix() || !std::isfinite(lat) || !std::isfinite(lon) || lat < -90.0 || lat > 90.0 ||
        lon < -180.0 || lon > 180.0 || (std::fabs(lat) < 1e-6 && std::fabs(lon) < 1e-6)) {
      return std::nullopt;
    }
    Fix fix;
    fix.lat = lat;
    fix.lon = lon;
    fix.accuracy_m = gps.getHorizontalAccuracy();
    fix.bearing_deg = gps.getBearingDeg();
    fix.speed_ms = gps.getSpeed();
    fix.fix_ms = gps.getUnixTimestampMillis();
    fix.log_mono_ns = event.getLogMonoTime();
    return fix;
  } catch (...) {
    return std::nullopt;
  }
}

QueuePeek::QueuePeek(std::string path) : ring_(std::move(path)) {}

QueuePeek::~QueuePeek() = default;

std::optional<std::vector<uint8_t>> QueuePeek::newest() {
  if (!ring_.refresh()) return std::nullopt;
  const char* data = ring_.data();
  const uint64_t data_size = ring_.data_size();

  // The writer stores each message's size and bytes before it moves the write pointer past them,
  // so everything from the start of this lap up to the pointer is whole.
  const uint64_t pointer = ring_.write_pointer();
  const uint32_t lap = static_cast<uint32_t>(pointer >> 32);
  const uint64_t end = pointer & 0xFFFFFFFFu;
  // Just wrapped: nothing in this lap yet, and the start of the last one may be being written over.
  if (end == 0 || end > data_size) return std::nullopt;

  uint64_t offset = 0;
  uint64_t newest_offset = 0;
  int64_t newest_size = -1;
  size_t steps = 0;
  while (offset < end) {
    if (++steps > kMaxWalkSteps || offset + sizeof(int64_t) > end) return std::nullopt;
    const int64_t size = load_i64(data + offset);
    if (size <= 0 || size > kMaxMessageBytes || offset + sizeof(int64_t) + static_cast<uint64_t>(size) > end) {
      return std::nullopt;
    }
    newest_offset = offset;
    newest_size = size;
    offset = align8(offset + sizeof(int64_t) + static_cast<uint64_t>(size));
  }
  if (offset != end || newest_size < 0) return std::nullopt;

  std::vector<uint8_t> message(static_cast<size_t>(newest_size));
  std::memcpy(message.data(), data + newest_offset + sizeof(int64_t), message.size());
  std::atomic_thread_fence(std::memory_order_acquire);

  // Keep the copy only if the writer can't have come round the ring and over it meanwhile.
  const uint64_t after = ring_.write_pointer();
  const uint32_t after_lap = static_cast<uint32_t>(after >> 32);
  const uint64_t after_end = after & 0xFFFFFFFFu;
  const bool intact = after_lap == lap || (after_lap == lap + 1 && after_end + kInFlightMarginBytes <= newest_offset);
  if (!intact) return std::nullopt;
  return message;
}

GpsPeek::GpsPeek(const std::string& msgq_dir)
    : external_(msgq_dir + "/msgq_gpsLocationExternal"), qualcomm_(msgq_dir + "/msgq_gpsLocation") {}

std::optional<Fix> GpsPeek::latest() {
  for (QueuePeek* queue : {&external_, &qualcomm_}) {
    if (auto message = queue->newest()) {
      if (auto fix = fix_from_event(message->data(), message->size())) {
        if (!latest_ || fix->fix_ms >= latest_->fix_ms) latest_ = fix;
      }
    }
  }
  return latest_;
}

}  // namespace commaview::gps
