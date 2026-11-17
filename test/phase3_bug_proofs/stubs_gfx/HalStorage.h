#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

#define O_RDONLY 0

struct GfxTestEsp {
  uint32_t getFreeHeap() const { return 256 * 1024; }
  uint32_t getMaxAllocHeap() const { return 128 * 1024; }
  void restart() {}
};
inline GfxTestEsp ESP;

class HalFile {
 public:
  explicit operator bool() const { return false; }
  bool isOpen() const { return false; }
  void close() {}
  size_t fileSize() const { return 0; }
  size_t position() const { return 0; }
  bool seek(uint64_t) { return false; }
  bool seekSet(uint64_t) { return false; }
  bool seekCur(int64_t) { return false; }
  int read() { return -1; }
  int read(void*, size_t) { return -1; }
};

class HalStorage {
 public:
  bool openFileForRead(const char*, const char*, HalFile&) { return false; }
};
inline HalStorage Storage;
