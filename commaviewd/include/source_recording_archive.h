#pragma once

#include "http_server.h"

#include <string>

namespace commaview::runtime {

// The caller must authenticate and enforce offroad state before invoking this.
commaview::api::HttpResponse source_recording_archive_response(
    const std::string& request_path);
commaview::api::HttpResponse source_recording_current_response(
    const std::string& route_id);
commaview::api::HttpResponse source_recording_arm_response();
commaview::api::HttpResponse source_recording_disarm_response();

}  // namespace commaview::runtime
