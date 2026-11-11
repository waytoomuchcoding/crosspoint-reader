#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace iso8601 {

// Parses an ISO-8601 timestamp ("YYYY-MM-DDTHH:MM:SSZ", "YYYY-MM-DD HH:MM:SS", or with fractional
// seconds and +/-HH:MM / +/-HHMM / +/-HH offset) into Unix UTC epoch seconds.
// Returns 0 on malformed dates, out-of-range calendar components, or trailing garbage.
inline int64_t parseUtc(std::string_view raw) {
  while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) {
    raw.remove_prefix(1);
  }
  while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) {
    raw.remove_suffix(1);
  }
  if (raw.size() < 19) return 0;

  auto parseDigits = [&](size_t pos, size_t count, int& out) -> bool {
    if (pos + count > raw.size()) return false;
    int val = 0;
    for (size_t i = 0; i < count; ++i) {
      const char c = raw[pos + i];
      if (c < '0' || c > '9') return false;
      val = val * 10 + (c - '0');
    }
    out = val;
    return true;
  };

  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (!parseDigits(0, 4, year) || raw[4] != '-' || !parseDigits(5, 2, month) || raw[7] != '-' ||
      !parseDigits(8, 2, day) || (raw[10] != 'T' && raw[10] != 't' && raw[10] != ' ') || !parseDigits(11, 2, hour) ||
      raw[13] != ':' || !parseDigits(14, 2, minute) || raw[16] != ':' || !parseDigits(17, 2, second)) {
    return 0;
  }

  if (year < 1970 || month < 1 || month > 12 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
      second > 60) {
    return 0;
  }

  const bool isLeap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
  static constexpr int kDaysInMonth[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int maxDay = (month == 2 && isLeap) ? 29 : kDaysInMonth[month - 1];
  if (day < 1 || day > maxDay) {
    return 0;
  }

  // Howard Hinnant's days_from_civil algorithm
  const int y = year - (month <= 2 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const auto m = static_cast<unsigned>(month);
  const auto d = static_cast<unsigned>(day);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;

  int64_t epoch = days * 86400 + static_cast<int64_t>(hour) * 3600 + static_cast<int64_t>(minute) * 60 + second;

  size_t pos = 19;
  if (pos < raw.size() && raw[pos] == '.') {
    ++pos;
    if (pos >= raw.size() || raw[pos] < '0' || raw[pos] > '9') return 0;
    while (pos < raw.size() && raw[pos] >= '0' && raw[pos] <= '9') {
      ++pos;
    }
  }

  if (pos == raw.size()) {
    return epoch > 0 ? epoch : 0;
  }
  if (pos + 1 == raw.size() && (raw[pos] == 'Z' || raw[pos] == 'z')) {
    return epoch > 0 ? epoch : 0;
  }
  if (raw[pos] == '+' || raw[pos] == '-') {
    const int sign = (raw[pos] == '+') ? 1 : -1;
    ++pos;
    int tzHour = 0, tzMin = 0;
    if (!parseDigits(pos, 2, tzHour) || tzHour > 23) return 0;
    pos += 2;
    if (pos < raw.size()) {
      if (raw[pos] == ':') ++pos;
      if (!parseDigits(pos, 2, tzMin) || tzMin > 59) return 0;
      pos += 2;
    }
    if (pos != raw.size()) return 0;
    epoch -= static_cast<int64_t>(sign) * (static_cast<int64_t>(tzHour) * 3600 + static_cast<int64_t>(tzMin) * 60);
    return epoch > 0 ? epoch : 0;
  }

  return 0;
}

}  // namespace iso8601
