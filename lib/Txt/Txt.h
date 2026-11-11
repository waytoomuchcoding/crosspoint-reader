#pragma once

#include <Print.h>
#include <TextEncoding.h>

#include <memory>
#include <string>
#include <string_view>

class BookMetadataCache;

class Txt {
 public:
  static bool isTxtOrMd(std::string_view path);
  static std::string getBookCachePath(const std::string& filepath, std::string_view cacheDir = "/.crosspoint");
  static TextEncoding readEncodingOverride(const std::string& cachePath);
  static bool writeEncodingOverride(const std::string& cachePath, TextEncoding enc);
  static TextEncoding detectFileEncoding(const std::string& filepath, const std::string& cachePath = "",
                                         bool* hasCjkOut = nullptr);
  static bool readEncodingPreview(const std::string& filepath, size_t byteOffset, TextEncoding encoding,
                                  std::string& utf8PreviewOut, TextEncoding* detectedOut = nullptr);
  static bool validateCache(const std::string& filepath, const std::string& cachePath, size_t cachedSize);
  static void invalidateCache(const std::string& cachePath);
  static void removeCachedCoverBmps(const std::string& cachePath);
  static void resolveTitleAndAuthor(const std::string& filepath, std::string& titleOut, std::string& authorOut);
  static bool streamTxtToHtml(const std::string& filepath, Print& out, const std::string& cachePath = "");
  static std::string findCompanionCoverImage(const std::string& filepath);
  static bool convertCoverImageToBmp(const std::string& imagePath, const std::string& destBmpPath, int thumbHeight = 0,
                                     bool cropped = false, bool originalThresholds = false, int thumbWidth = 0);
  static bool buildTxtCache(const std::string& filepath, const std::string& cachePath,
                            std::unique_ptr<BookMetadataCache>& bookMetadataCache, bool preserveContentCache = false);
};
