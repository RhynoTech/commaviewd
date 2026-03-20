#include "raw_event_metadata.h"

#include "cereal/gen/cpp/log.capnp.h"

namespace commaview::telemetry {
namespace {

uint32_t read_le32(const uint8_t* ptr) {
  return static_cast<uint32_t>(ptr[0]) |
         (static_cast<uint32_t>(ptr[1]) << 8) |
         (static_cast<uint32_t>(ptr[2]) << 16) |
         (static_cast<uint32_t>(ptr[3]) << 24);
}

uint64_t read_le64(const uint8_t* ptr) {
  return static_cast<uint64_t>(ptr[0]) |
         (static_cast<uint64_t>(ptr[1]) << 8) |
         (static_cast<uint64_t>(ptr[2]) << 16) |
         (static_cast<uint64_t>(ptr[3]) << 24) |
         (static_cast<uint64_t>(ptr[4]) << 32) |
         (static_cast<uint64_t>(ptr[5]) << 40) |
         (static_cast<uint64_t>(ptr[6]) << 48) |
         (static_cast<uint64_t>(ptr[7]) << 56);
}

int32_t sign_extend_30(uint32_t value) {
  if ((value & 0x20000000U) != 0U) {
    return static_cast<int32_t>(value | 0xC0000000U);
  }
  return static_cast<int32_t>(value);
}

}  // namespace

bool extract_log_mono_time_from_raw_event(const uint8_t* raw, size_t raw_size, uint64_t* log_mono_time) {
  if (raw == nullptr || log_mono_time == nullptr || raw_size < 16) return false;

  const uint32_t segment_count = read_le32(raw) + 1U;
  if (segment_count == 0U) return false;

  const size_t table_words = 1U + static_cast<size_t>(segment_count) + ((segment_count % 2U) == 0U ? 1U : 0U);
  const size_t table_bytes = table_words * 4U;
  if (table_bytes > raw_size) return false;

  const size_t segment0_bytes = static_cast<size_t>(read_le32(raw + 4)) * 8U;
  if (segment0_bytes < 8U) return false;
  if (table_bytes + segment0_bytes > raw_size) return false;

  const size_t root_ptr_offset = table_bytes;
  const uint64_t root_ptr = read_le64(raw + root_ptr_offset);
  if ((root_ptr & 0x3ULL) != 0ULL) return false;

  const int32_t offset_words = sign_extend_30(static_cast<uint32_t>((root_ptr >> 2) & 0x3FFFFFFFULL));
  const uint16_t data_words = static_cast<uint16_t>((root_ptr >> 32) & 0xFFFFULL);
  if (data_words < 1U) return false;

  const int64_t data_offset = static_cast<int64_t>(root_ptr_offset) + 8 + static_cast<int64_t>(offset_words) * 8;
  if (data_offset < static_cast<int64_t>(table_bytes)) return false;
  if (static_cast<size_t>(data_offset) + 8U > table_bytes + segment0_bytes) return false;

  *log_mono_time = read_le64(raw + data_offset);
  return true;
}

uint16_t event_which_for_service_index(int service_index) {
  switch (service_index) {
    case 0: return static_cast<uint16_t>(cereal::Event::CAR_STATE);
    case 1: return static_cast<uint16_t>(cereal::Event::SELFDRIVE_STATE);
    case 2: return static_cast<uint16_t>(cereal::Event::DEVICE_STATE);
    case 3: return static_cast<uint16_t>(cereal::Event::LIVE_CALIBRATION);
    case 4: return static_cast<uint16_t>(cereal::Event::RADAR_STATE);
    case 5: return static_cast<uint16_t>(cereal::Event::MODEL_V2);
    case 6: return static_cast<uint16_t>(cereal::Event::CAR_CONTROL);
    case 7: return static_cast<uint16_t>(cereal::Event::CAR_OUTPUT);
    case 8: return static_cast<uint16_t>(cereal::Event::LIVE_PARAMETERS);
    case 9: return static_cast<uint16_t>(cereal::Event::DRIVER_MONITORING_STATE);
    case 10: return static_cast<uint16_t>(cereal::Event::DRIVER_STATE_V2);
    case 11: return static_cast<uint16_t>(cereal::Event::ONROAD_EVENTS);
    case 12: return static_cast<uint16_t>(cereal::Event::ROAD_CAMERA_STATE);
    default: return 0;
  }
}

}  // namespace commaview::telemetry
