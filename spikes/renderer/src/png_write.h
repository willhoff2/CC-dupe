// Minimal PNG writer (stored deflate blocks; no external dependency).
#pragma once

#include <cstdint>
#include <string>

namespace spike {

// `rgba` is tightly packed 8-bit RGBA, top row first, width*height*4 bytes.
bool Write_Png(const std::string& path, const std::string& rgba, uint32_t width,
               uint32_t height);

} // namespace spike
