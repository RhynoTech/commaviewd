#include "router.h"

namespace commaview::video {

cereal::Event::Which expected_video_which_for_port(int port, bool dev_mode) {
  if (dev_mode) {
    if (port == 8200) return cereal::Event::LIVESTREAM_ROAD_ENCODE_DATA;
    if (port == 8201) return cereal::Event::LIVESTREAM_WIDE_ROAD_ENCODE_DATA;
    return cereal::Event::LIVESTREAM_DRIVER_ENCODE_DATA;
  }
  if (port == 8200) return cereal::Event::ROAD_ENCODE_DATA;
  if (port == 8201) return cereal::Event::WIDE_ROAD_ENCODE_DATA;
  return cereal::Event::DRIVER_ENCODE_DATA;
}

cereal::EncodeData::Reader read_encode_data(cereal::Event::Reader event, int port, bool dev_mode) {
  if (dev_mode) {
    if (port == 8200) return event.getLivestreamRoadEncodeData();
    if (port == 8201) return event.getLivestreamWideRoadEncodeData();
    return event.getLivestreamDriverEncodeData();
  }

  if (port == 8200) return event.getRoadEncodeData();
  if (port == 8201) return event.getWideRoadEncodeData();
  return event.getDriverEncodeData();
}

}  // namespace commaview::video
