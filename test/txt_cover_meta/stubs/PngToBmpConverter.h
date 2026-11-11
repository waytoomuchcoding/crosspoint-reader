#pragma once
#include <HalStorage.h>
#include <Print.h>

class PngToBmpConverter {
 public:
  static bool pngFileToBmpStream(HalFile&, Print& out, bool = true, bool = false) {
    const char hdr[] = "PNG2";
    return out.write(reinterpret_cast<const uint8_t*>(hdr), 4) == 4;
  }
  static bool pngFileTo1BitBmpStreamWithSize(HalFile&, Print& out, int w, int h) {
    char tag[32];
    const int n = snprintf(tag, sizeof(tag), "PNG1_%dx%d", w, h);
    return out.write(reinterpret_cast<const uint8_t*>(tag), static_cast<size_t>(n)) == static_cast<size_t>(n);
  }
};
