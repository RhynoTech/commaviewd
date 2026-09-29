#include "router.h"

namespace commaview::video {

cereal::Event::Which expected_video_which_for_port(int port, bool livestream) {
#if defined(COMMAVIEW_CURRENT_VIDEO_SCHEMA)
  if (livestream) {
    if (port == 8200) return cereal::Event::LIVESTREAM_NARROW_ROAD_ENCODE_DATA;
    if (port == 8201) return cereal::Event::LIVESTREAM_WIDE_ROAD_ENCODE_DATA;
    return cereal::Event::LIVESTREAM_CABIN_ENCODE_DATA;
  }
  if (port == 8200) return cereal::Event::NARROW_ROAD_ENCODE_DATA;
  if (port == 8201) return cereal::Event::WIDE_ROAD_ENCODE_DATA;
  return cereal::Event::CABIN_ENCODE_DATA;
#else
  if (livestream) {
    if (port == 8200) return cereal::Event::LIVESTREAM_ROAD_ENCODE_DATA;
    if (port == 8201) return cereal::Event::LIVESTREAM_WIDE_ROAD_ENCODE_DATA;
    return cereal::Event::LIVESTREAM_DRIVER_ENCODE_DATA;
  }
  if (port == 8200) return cereal::Event::ROAD_ENCODE_DATA;
  if (port == 8201) return cereal::Event::WIDE_ROAD_ENCODE_DATA;
  return cereal::Event::DRIVER_ENCODE_DATA;
#endif
}

cereal::EncodeData::Reader read_encode_data(cereal::Event::Reader event, int port, bool livestream) {
#if defined(COMMAVIEW_CURRENT_VIDEO_SCHEMA)
  if (livestream) {
    if (port == 8200) return event.getLivestreamNarrowRoadEncodeData();
    if (port == 8201) return event.getLivestreamWideRoadEncodeData();
    return event.getLivestreamCabinEncodeData();
  }

  if (port == 8200) return event.getNarrowRoadEncodeData();
  if (port == 8201) return event.getWideRoadEncodeData();
  return event.getCabinEncodeData();
#else
  if (livestream) {
    if (port == 8200) return event.getLivestreamRoadEncodeData();
    if (port == 8201) return event.getLivestreamWideRoadEncodeData();
    return event.getLivestreamDriverEncodeData();
  }

  if (port == 8200) return event.getRoadEncodeData();
  if (port == 8201) return event.getWideRoadEncodeData();
  return event.getDriverEncodeData();
#endif
}

}  // namespace commaview::video
