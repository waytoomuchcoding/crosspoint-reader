#include "ReaderFontSizes.h"

#include <SdCardFont.h>

#include <cstdio>
#include <iterator>

std::vector<uint8_t> readerFontPointSizes(const SdCardFontRegistry* registry, const char* sdFamilyName) {
  if (registry && sdFamilyName && sdFamilyName[0] != '\0') {
    if (const auto* family = registry->findFamily(sdFamilyName)) {
      if (family->vector) {
        return {std::begin(VECTOR_READER_POINT_SIZES), std::end(VECTOR_READER_POINT_SIZES)};
      }
      auto sizes = family->availableSizes();
      if (!sizes.empty()) return sizes;
    }
  }
  return {std::begin(BUILTIN_READER_POINT_SIZES), std::end(BUILTIN_READER_POINT_SIZES)};
}

std::string formatReaderFontSizeLabel(const SdCardFontRegistry* registry, const char* sdFamilyName, const uint8_t pt) {
  char buf[24];
  if (registry && sdFamilyName && sdFamilyName[0] != '\0') {
    if (const auto* family = registry->findFamily(sdFamilyName); family && !family->vector) {
      if (const auto* file = family->findFile(pt)) {
        char path[SdCardFontFamilyInfo::PATH_BUFFER_SIZE];
        if (family->filePath(*file, path, sizeof(path))) {
          if (const uint8_t px = SdCardFont::peekBitonalAdvanceY(path); px > 0) {
            snprintf(buf, sizeof(buf), "%u pt (%u px)", pt, px);
            return buf;
          }
        }
      }
    }
  }
  snprintf(buf, sizeof(buf), "%u pt", pt);
  return buf;
}

uint8_t snapToNearestPointSize(const uint8_t* sizes, const size_t count, const uint8_t pt) {
  if (!sizes || count == 0) return pt;

  uint8_t best = sizes[0];
  uint8_t bestDelta = best > pt ? best - pt : pt - best;
  for (size_t i = 1; i < count; i++) {
    const uint8_t delta = sizes[i] > pt ? sizes[i] - pt : pt - sizes[i];
    // Strictly-less keeps the smaller size on a tie, since `sizes` is ascending.
    if (delta < bestDelta) {
      best = sizes[i];
      bestDelta = delta;
    }
  }
  return best;
}
