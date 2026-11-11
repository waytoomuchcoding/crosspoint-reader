#pragma once
#include <HalStorage.h>
#include <Print.h>

class JpegToBmpConverter {
 public:
  static bool jpegFileToBmpStream(HalFile&, Print& out, bool = true, bool = false) {
    const char hdr[] = "JPG2";
    return out.write(reinterpret_cast<const uint8_t*>(hdr), 4) == 4;
  }
  static bool jpegFileTo1BitBmpStreamWithSize(HalFile&, Print& out, int w, int h) {
    char tag[32];
    const int n = snprintf(tag, sizeof(tag), "JPG1_%dx%d", w, h);
    return out.write(reinterpret_cast<const uint8_t*>(tag), static_cast<size_t>(n)) == static_cast<size_t>(n);
  }
};
