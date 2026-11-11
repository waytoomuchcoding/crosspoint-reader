#pragma once

#include <WString.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Stream {};

namespace hostfs {

struct MemNode {
  bool isDir = false;
  std::vector<uint8_t> bytes;
};

inline std::map<std::string, std::shared_ptr<MemNode>>& files() {
  static std::map<std::string, std::shared_ptr<MemNode>> table;
  return table;
}

inline void reset() { files().clear(); }

inline void writeString(const std::string& path, const std::string& content) {
  auto node = std::make_shared<MemNode>();
  node->bytes.assign(content.begin(), content.end());
  files()[path] = std::move(node);
}

inline std::string readString(const std::string& path) {
  auto it = files().find(path);
  if (it == files().end() || !it->second || it->second->isDir) return {};
  return std::string(it->second->bytes.begin(), it->second->bytes.end());
}

}  // namespace hostfs

struct FsFile {
  std::shared_ptr<hostfs::MemNode> node;
  std::string path;
  size_t pos = 0;

  explicit operator bool() const { return static_cast<bool>(node); }
  bool isOpen() const { return static_cast<bool>(node); }
  bool isDirectory() const { return node && node->isDir; }
  FsFile openNextFile() { return {}; }
  size_t getName(char* out, size_t cap) const {
    const std::string name = path.substr(path.find_last_of('/') + 1);
    if (out && cap > 0) {
      const size_t n = std::min(name.size(), cap - 1);
      std::memcpy(out, name.data(), n);
      out[n] = '\0';
    }
    return name.size();
  }
  size_t size() const { return node ? node->bytes.size() : 0; }
  size_t fileSize() const { return size(); }
  int read(void* dst, size_t n) {
    if (!node || n == 0) return 0;
    const size_t avail = node->bytes.size() - std::min(pos, node->bytes.size());
    const size_t toCopy = std::min(n, avail);
    if (toCopy > 0) {
      std::memcpy(dst, node->bytes.data() + pos, toCopy);
      pos += toCopy;
    }
    return static_cast<int>(toCopy);
  }
  size_t write(const void* src, size_t n) {
    if (!node || !src || n == 0) return 0;
    if (pos + n > node->bytes.size()) {
      node->bytes.resize(pos + n);
    }
    std::memcpy(node->bytes.data() + pos, src, n);
    pos += n;
    return n;
  }
  bool close() {
    node.reset();
    pos = 0;
    return true;
  }
};

using HalFile = FsFile;

enum oflag_t { O_RDONLY = 0, O_WRONLY = 1, O_CREAT = 2, O_TRUNC = 4 };

class HalStorage {
 public:
  bool exists(const char* p) const {
    if (!p) return false;
    return hostfs::files().count(p) != 0;
  }
  bool mkdir(const char* p, bool = true) {
    if (!p) return false;
    auto& f = hostfs::files()[p];
    if (!f) f = std::make_shared<hostfs::MemNode>();
    f->isDir = true;
    return true;
  }
  bool ensureDirectoryExists(const char* p) { return mkdir(p, true); }
  FsFile open(const char* p, int = 0) const {
    FsFile f;
    if (!p) return f;
    auto it = hostfs::files().find(p);
    if (it != hostfs::files().end()) {
      f.node = it->second;
      f.path = p;
    }
    return f;
  }
  bool remove(const char* p) {
    if (!p) return false;
    return hostfs::files().erase(p) != 0;
  }
  bool rename(const char* from, const char* to) {
    if (!from || !to || !exists(from) || exists(to)) return false;
    auto node = hostfs::files()[from];
    hostfs::files().erase(from);
    hostfs::files()[to] = std::move(node);
    return true;
  }
  bool replaceFile(const char* from, const char* to) {
    if (!from || !to || !exists(from)) return false;
    auto node = hostfs::files()[from];
    hostfs::files().erase(from);
    hostfs::files()[to] = std::move(node);
    return true;
  }
  bool writeFile(const char* p, const String& s) {
    if (!p) return false;
    hostfs::writeString(p, std::string(s.c_str(), s.length()));
    return true;
  }
  String readFile(const char* p) const {
    if (!p) return {};
    return String(hostfs::readString(p));
  }
  bool openFileForRead(const char*, const char* p, FsFile& out) {
    if (!p) return false;
    auto it = hostfs::files().find(p);
    if (it == hostfs::files().end() || !it->second || it->second->isDir) return false;
    out.node = it->second;
    out.pos = 0;
    return true;
  }
  bool openFileForRead(const char* mod, const std::string& p, FsFile& out) {
    return openFileForRead(mod, p.c_str(), out);
  }
  bool openFileForWrite(const char*, const char* p, FsFile& out) {
    if (!p) return false;
    auto node = std::make_shared<hostfs::MemNode>();
    hostfs::files()[p] = node;
    out.node = std::move(node);
    out.pos = 0;
    return true;
  }
  bool openFileForWrite(const char* mod, const std::string& p, FsFile& out) {
    return openFileForWrite(mod, p.c_str(), out);
  }
  bool readFileToString(const char*, const char* p, size_t maxBytes, std::string& out) {
    if (!p) return false;
    auto it = hostfs::files().find(p);
    if (it == hostfs::files().end() || !it->second || it->second->isDir) return false;
    if (it->second->bytes.size() > maxBytes) return false;
    out.assign(it->second->bytes.begin(), it->second->bytes.end());
    return true;
  }
  bool readFileToString(const char* mod, const std::string& p, size_t maxBytes, std::string& out) {
    return readFileToString(mod, p.c_str(), maxBytes, out);
  }
};

inline HalStorage Storage;
