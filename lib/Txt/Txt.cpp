#include "Txt.h"

#include <Epub/BookMetadataCache.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <TextEncoding.h>
#include <Utf8.h>

#include <cstring>
#include <functional>

#include "TxtEncodingSupport.h"
#include "TxtToHtml.h"

bool Txt::isTxtOrMd(std::string_view path) {
  return FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path);
}

std::string Txt::getBookCachePath(const std::string& filepath, std::string_view cacheDir) {
  return std::string(cacheDir) + "/epub_" + std::to_string(std::hash<std::string>{}(filepath));
}

TextEncoding Txt::readEncodingOverride(const std::string& cachePath) { return txtenc::readEncodingOverride(cachePath); }

bool Txt::writeEncodingOverride(const std::string& cachePath, TextEncoding enc) {
  return txtenc::writeEncodingOverride(cachePath, enc);
}

TextEncoding Txt::detectFileEncoding(const std::string& filepath, const std::string& cachePath, bool* hasCjkOut) {
  return txtenc::detectFileEncoding(filepath, cachePath, hasCjkOut);
}

bool Txt::readEncodingPreview(const std::string& filepath, size_t byteOffset, TextEncoding encoding,
                              std::string& utf8PreviewOut, TextEncoding* detectedOut) {
  return txtenc::readEncodingPreview(filepath, byteOffset, encoding, utf8PreviewOut, detectedOut);
}

std::string Txt::findCompanionCoverImage(const std::string& filepath) {
  std::string folder = FsHelpers::extractFolderPath(filepath);

  std::string baseName = FsHelpers::getFileNameWithoutExtension(filepath);
  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};

  for (const auto& ext : extensions) {
    std::string coverPath = folder + "/" + baseName + ext;
    if (Storage.exists(coverPath.c_str())) return coverPath;
  }

  const char* coverNames[] = {"cover", "Cover", "COVER"};
  for (const auto& name : coverNames) {
    for (const auto& ext : extensions) {
      std::string coverPath = folder + "/" + std::string(name) + ext;
      if (Storage.exists(coverPath.c_str())) return coverPath;
    }
  }

  return "";
}

bool Txt::convertCoverImageToBmp(const std::string& imagePath, const std::string& destBmpPath, int thumbHeight,
                                 bool cropped, bool originalThresholds) {
  if (imagePath.empty() || destBmpPath.empty() || !Storage.exists(imagePath.c_str())) return false;

  const bool isBmp = FsHelpers::hasBmpExtension(imagePath);
  const bool isJpg = FsHelpers::hasJpgExtension(imagePath);
  const bool isPng = FsHelpers::hasPngExtension(imagePath);
  if (!isBmp && !isJpg && !isPng) return false;

  const std::string tmpPath = destBmpPath + ".tmp";
  HalFile src;
  if (!Storage.openFileForRead("TXT", imagePath, src)) {
    return false;
  }
  HalFile dst;
  if (!Storage.openFileForWrite("TXT", tmpPath, dst)) {
    src.close();
    return false;
  }

  bool success = false;
  if (isBmp) {
    uint8_t buf[512];
    success = true;
    while (src.available()) {
      size_t bytesRead = src.read(buf, sizeof(buf));
      if (dst.write(buf, bytesRead) != bytesRead) {
        success = false;
        break;
      }
    }
  } else if (thumbHeight > 0) {
    const int targetWidth = static_cast<int>(thumbHeight * 0.6);
    const int targetHeight = thumbHeight;
    if (isJpg) {
      success = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(src, dst, targetWidth, targetHeight);
    } else if (isPng) {
      success = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(src, dst, targetWidth, targetHeight);
    }
  } else {
    if (isJpg) {
      success = JpegToBmpConverter::jpegFileToBmpStream(src, dst, cropped, originalThresholds);
    } else if (isPng) {
      success = PngToBmpConverter::pngFileToBmpStream(src, dst, cropped, originalThresholds);
    }
  }

  const bool srcClosed = src.close();
  const bool dstClosed = dst.close();
  if (!success || !srcClosed || !dstClosed) {
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (Storage.exists(destBmpPath.c_str()) && !Storage.remove(destBmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), destBmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
    return false;
  }

  return true;
}

bool Txt::streamTxtToHtml(const std::string& filepath, Print& out, const std::string& cachePath) {
  [[maybe_unused]] const uint32_t t0 = millis();
  const TextEncoding activeEncoding = detectFileEncoding(filepath, cachePath);

  HalFile src;
  if (!Storage.openFileForRead("TXT", filepath, src)) {
    LOG_ERR("TXT", "Failed to open TXT/MD for streaming: %s", filepath.c_str());
    return false;
  }

  [[maybe_unused]] const size_t srcSize = src.size();
  LOG_DBG("TXT", "Converting TXT/MD to HTML (%s): %s (%zu bytes)", textencoding::encodingShortName(activeEncoding),
          filepath.c_str(), srcSize);

  const bool ok = TxtToHtml::stream(
      filepath, &src, [](void* ctx, uint8_t* buf, size_t size) { return static_cast<HalFile*>(ctx)->read(buf, size); },
      out, activeEncoding);

  LOG_DBG("TXT", "Converted TXT/MD to HTML in %lu ms (%zu bytes in)", millis() - t0, srcSize);
  return ok;
}

void Txt::invalidateCache(const std::string& cachePath) {
  Storage.removeDir((cachePath + "/html").c_str());
  Storage.removeDir((cachePath + "/sections").c_str());
}

bool Txt::validateCache(const std::string& filepath, const std::string& cachePath, size_t cachedSize) {
  bool valid = true;

  const std::string htmlPath = cachePath + "/html/0.html";
  if (Storage.exists(htmlPath.c_str())) {
    HalFile htmlFile;
    if (Storage.openFileForRead("TXT", htmlPath, htmlFile)) {
      char header[80] = {0};
      const int bytesRead = htmlFile.read(header, sizeof(header) - 1);
      if (bytesRead > 0) {
        header[bytesRead] = '\0';
        if (strstr(header, TxtToHtml::cacheVersionTag(filepath)) == nullptr) {
          valid = false;
        }
      } else {
        valid = false;
      }
    } else {
      valid = false;
    }
  }

  if (valid && cachedSize > 0) {
    HalFile rawFile;
    if (Storage.openFileForRead("TXT", filepath, rawFile)) {
      if (rawFile.size() != cachedSize) {
        valid = false;
      }
    } else {
      valid = false;
    }
  }

  if (!valid) {
    invalidateCache(cachePath);
  }
  return valid;
}

bool Txt::buildTxtCache(const std::string& filepath, const std::string& cachePath,
                        std::unique_ptr<BookMetadataCache>& bookMetadataCache) {
  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  } else {
    invalidateCache(cachePath);
  }

  bool hasCjk = false;
  const TextEncoding activeEncoding = detectFileEncoding(filepath, cachePath, &hasCjk);

  if (!bookMetadataCache->beginWrite() || !bookMetadataCache->beginContentOpfPass()) {
    return false;
  }

  size_t txtSize = 0;
  HalFile rawFile;
  if (Storage.openFileForRead("TXT", filepath, rawFile)) {
    txtSize = rawFile.size();
  }
  bookMetadataCache->createSpineEntry("content.html", cachePath + "/sections/0.bin", txtSize);

  if (!bookMetadataCache->endContentOpfPass() || !bookMetadataCache->beginTocPass()) {
    return false;
  }

  std::string title = FsHelpers::getFileNameWithoutExtension(filepath);
  const size_t tocEntriesCreated = txtenc::scanChapterHeadings(filepath, activeEncoding, *bookMetadataCache, hasCjk);
  if (tocEntriesCreated == 0) {
    bookMetadataCache->createTocEntry(title, "content.html", "", 0);
  }

  if (!bookMetadataCache->endTocPass()) {
    return false;
  }

  BookMetadataCache::BookMetadata bookMetadata;
  bookMetadata.title = utf8ComposeNfc(title);
  bookMetadata.language = hasCjk ? "zh" : "en";

  std::string companionCover = findCompanionCoverImage(filepath);
  if (!companionCover.empty()) {
    bookMetadata.coverItemHref = companionCover;
  }

  if (!bookMetadataCache->endWrite(bookMetadata)) {
    return false;
  }
  bookMetadataCache->cleanupTmpFiles();

  bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  return bookMetadataCache && bookMetadataCache->load();
}
