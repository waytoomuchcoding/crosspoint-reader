#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

class String {
 public:
  String() = default;
  String(const char* s) : value_(s ? s : "") {}
  String(const std::string& s) : value_(s) {}

  const char* c_str() const { return value_.c_str(); }
  const char* begin() const { return value_.data(); }
  const char* end() const { return value_.data() + value_.size(); }
  size_t length() const { return value_.length(); }
  bool isEmpty() const { return value_.empty(); }
  void clear() { value_.clear(); }

  size_t write(uint8_t c) {
    value_.push_back(static_cast<char>(c));
    return 1;
  }

  size_t write(const uint8_t* s, size_t n) {
    if (!s || n == 0) return 0;
    value_.append(reinterpret_cast<const char*>(s), n);
    return n;
  }

  bool operator==(const char* other) const { return other && value_ == other; }
  bool operator==(const String& other) const { return value_ == other.value_; }

 private:
  std::string value_;
};
