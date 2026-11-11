#pragma once
#include <HalStorage.h>
#include <Print.h>

class BmpToBmpConverter {
 public:
  static bool bmpFileToBmpStream(HalFile&, Print& out, bool, bool) {
    const char hdr[] = "BMP2";
    return out.write(reinterpret_cast<const uint8_t*>(hdr), 4) == 4;
  }
  static bool bmpFileTo1BitBmpStreamWithSize(HalFile&, Print& out, int w, int h, bool = true) {
    char tag[32];
    const int n = snprintf(tag, sizeof(tag), "BMP1_%dx%d", w, h);
    return out.write(reinterpret_cast<const uint8_t*>(tag), static_cast<size_t>(n)) == static_cast<size_t>(n);
  }
};
