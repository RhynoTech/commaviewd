#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace commaview::net {

void put_be32(uint8_t* buf, uint32_t val);
uint32_t read_be32(const uint8_t* buf);

bool send_all(int fd, const void* data, size_t len);
bool send_frame(int fd, const uint8_t* payload, size_t payload_len);
bool send_meta_json(int fd, const std::string& json, uint8_t msg_meta_type);

}  // namespace commaview::net
