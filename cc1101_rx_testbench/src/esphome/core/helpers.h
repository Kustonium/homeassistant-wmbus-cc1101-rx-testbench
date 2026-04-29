#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {

inline std::string format_hex(const uint8_t *data, size_t len) {
  static const char *hex = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    const uint8_t b = data[i];
    out.push_back(hex[b >> 4]);
    out.push_back(hex[b & 0x0F]);
  }
  return out;
}

inline std::string format_hex(const std::vector<uint8_t> &data) {
  return format_hex(data.data(), data.size());
}

}  // namespace esphome
