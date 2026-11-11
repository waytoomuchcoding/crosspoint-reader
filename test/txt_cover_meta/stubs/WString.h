#pragma once

#include <string>

class String {
 public:
  String() = default;
  String(const char* s) : value_(s ? s : "") {}

  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.length(); }

 private:
  std::string value_;
};
