#pragma once

#include <cstddef>
#include <functional>

namespace freeink {

enum class MemPool : unsigned char { Internal, Psram, Default };

struct MemorySink {
  const char* name = nullptr;
  int priority = 0;
  std::function<size_t(size_t)> reclaim;
};

class MemoryManager {
 public:
  static MemoryManager& instance() {
    static MemoryManager inst;
    return inst;
  }
  bool ensureFree(size_t, MemPool = MemPool::Default) { return true; }
  void registerSink(MemorySink) {}
  size_t freeBytes(MemPool = MemPool::Default) const { return 1024 * 1024; }

 private:
  MemoryManager() = default;
};

}  // namespace freeink
