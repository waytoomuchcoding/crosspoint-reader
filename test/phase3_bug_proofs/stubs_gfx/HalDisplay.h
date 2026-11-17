#pragma once

#include <cassert>
#include <cstdint>

class HalDisplay {
 public:
  static constexpr uint16_t DISPLAY_WIDTH = 800;
  static constexpr uint16_t DISPLAY_HEIGHT = 480;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;
  enum RefreshMode { FAST_REFRESH, HALF_REFRESH, FULL_REFRESH };
  enum class GrayscaleMode { Overlay, Absolute };
  enum class GrayscaleBase { Separate, Combined };
  struct GrayscaleCapabilities {
    GrayscaleBase base = GrayscaleBase::Separate;
    bool asyncBase = false;
    bool stripUploads = false;
  };

  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode = GrayscaleMode::Overlay) const { return {}; }
  uint16_t getDisplayWidth() const { return DISPLAY_WIDTH; }
  uint16_t getDisplayHeight() const { return DISPLAY_HEIGHT; }
  uint16_t getDisplayWidthBytes() const { return DISPLAY_WIDTH_BYTES; }
  uint32_t getBufferSize() const { return BUFFER_SIZE; }
  uint8_t* getFrameBuffer() { return frameBuf_; }
  uint8_t* lendFrameBufferStorage(uint32_t* size) {
    if (size) *size = BUFFER_SIZE;
    return frameBuf_;
  }
  void returnFrameBufferStorage() {}

  bool isInverted() const { return false; }
  void drawPixel(int, int, bool) {}
  void drawImage(const uint8_t*, int, int, int, int) const {}
  void clearScreen(uint8_t = 0xFF) {}
  void displayBuffer(RefreshMode = FAST_REFRESH, bool = false) {}
  void displayBufferAsync(RefreshMode = FAST_REFRESH) {}
  void waitRefreshComplete() const {}
  bool supportsAsyncRefresh() const { return false; }
  void refreshDisplay(RefreshMode = FAST_REFRESH, bool = false) {}
  void copyGrayscaleLsbBuffers(const uint8_t*) {}
  void copyGrayscaleMsbBuffers(const uint8_t*) {}
  void displayGrayBuffer(bool = false) {}
  void displayGrayscaleBase(RefreshMode, bool) {}
  bool displayGrayscaleBase(GrayscaleMode, RefreshMode, bool) { return true; }
  void preconditionGrayscale() const {}
  void preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) const {}
  void writeGrayscalePlaneStrip(bool, const uint8_t*, uint16_t, uint16_t) const {}
  void setFramebuffer(const uint8_t*) {}
  void swapBuffers() {}
  void cleanupGrayscaleBuffers(const uint8_t*) {}

 private:
  uint8_t frameBuf_[BUFFER_SIZE] = {};
};
