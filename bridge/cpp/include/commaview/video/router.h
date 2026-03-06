#pragma once

#include "cereal/gen/cpp/log.capnp.h"

namespace commaview::video {

cereal::Event::Which expected_video_which_for_port(int port, bool dev_mode);
cereal::EncodeData::Reader read_encode_data(cereal::Event::Reader event, int port, bool dev_mode);

}  // namespace commaview::video
