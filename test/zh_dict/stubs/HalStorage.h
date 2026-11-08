#pragma once
// Host stand-in for the firmware's HalStorage: reads files from the local disk.

#include <sys/stat.h>

#include <cstdio>

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  bool openRead(const char* path) {
    close();
    file_ = std::fopen(path, "rb");
    return file_ != nullptr;
  }
  bool seekSet(size_t pos) { return file_ && std::fseek(file_, static_cast<long>(pos), SEEK_SET) == 0; }
  int read(void* buf, size_t count) { return file_ ? static_cast<int>(std::fread(buf, 1, count, file_)) : -1; }
  size_t fileSize() {
    if (!file_) return 0;
    const long at = std::ftell(file_);
    std::fseek(file_, 0, SEEK_END);
    const long end = std::ftell(file_);
    std::fseek(file_, at, SEEK_SET);
    return end > 0 ? static_cast<size_t>(end) : 0;
  }
  bool close() {
    if (!file_) return false;
    std::fclose(file_);
    file_ = nullptr;
    return true;
  }
  bool isOpen() const { return file_ != nullptr; }

 private:
  std::FILE* file_ = nullptr;
};

class HalStorage {
 public:
  bool exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) { return file.openRead(path); }
};

inline HalStorage Storage;
