#include "Txt.h"

#include <BmpToBmpConverter.h>
#include <Epub/BookMetadataCache.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <TextEncoding.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "TxtCoverMeta.h"
#include "TxtEncodingSupport.h"
#include "TxtToHtml.h"

namespace {

void appendFileStatStamp(std::string& out, const std::string& path) {
  if (path.empty() || !Storage.exists(path.c_str())) {
    out.append("none\n");
    return;
  }
  HalFile f;
  uint32_t sz = 0;
  uint32_t mtime = 0;
  if (Storage.openFileForRead("TXT", path, f)) {
    sz = static_cast<uint32_t>(f.size());
    mtime = f.modificationTime();
    f.close();
  }
  char statBuf[48];
  std::snprintf(statBuf, sizeof(statBuf), ":%lu:%lu\n", static_cast<unsigned long>(sz),
                static_cast<unsigned long>(mtime));
  out.append(path);
  out.append(statBuf);
}

std::string computeCoverMetaStamp(const std::string& filepath) {
  std::string stamp;
  stamp.reserve(160);
  stamp.append("cover=");
  appendFileStatStamp(stamp, Txt::findCompanionCoverImage(filepath));
  stamp.append("meta=");
  appendFileStatStamp(stamp, txtmeta::getBoundMetaJsonPath(filepath));
  return stamp;
}

void writeCoverMetaStamp(const std::string& filepath, const std::string& cachePath) {
  const std::string stamp = computeCoverMetaStamp(filepath);
  const std::string stampPath = cachePath + "/cover.stamp";
  HalFile f;
  if (Storage.openFileForWrite("TXT", stampPath, f)) {
    f.write(reinterpret_cast<const uint8_t*>(stamp.data()), stamp.size());
    f.close();
  }
}

bool isCoverMetaStampFresh(const std::string& filepath, const std::string& cachePath) {
  const std::string stampPath = cachePath + "/cover.stamp";
  if (!Storage.exists(stampPath.c_str())) {
    // Legacy cache written before cover.stamp: fresh only if neither a companion cover nor bound metadata exists.
    return Txt::findCompanionCoverImage(filepath).empty() &&
           !Storage.exists(txtmeta::getBoundMetaJsonPath(filepath).c_str());
  }
  HalFile f;
  if (!Storage.openFileForRead("TXT", stampPath, f)) {
    return false;
  }
  const std::string expected = computeCoverMetaStamp(filepath);
  if (f.size() != expected.size()) {
    f.close();
    return false;
  }
  char chunk[128];
  size_t compared = 0;
  while (compared < expected.size()) {
    const size_t want = std::min<size_t>(sizeof(chunk), expected.size() - compared);
    const int n = f.read(chunk, want);
    if (n <= 0 || std::memcmp(chunk, expected.data() + compared, static_cast<size_t>(n)) != 0) {
      f.close();
      return false;
    }
    compared += static_cast<size_t>(n);
  }
  f.close();
  return true;
}

}  // namespace

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

void Txt::resolveTitleAndAuthor(const std::string& filepath, std::string& titleOut, std::string& authorOut) {
  titleOut.clear();
  authorOut.clear();

  const std::string stem = FsHelpers::getFileNameWithoutExtension(filepath);
  const std::string metaPath = txtmeta::getBoundMetaJsonPath(filepath);
  txtmeta::BoundBookMeta meta;
  if (txtmeta::readMetaJson(metaPath, meta)) {
    titleOut = !meta.title.empty() ? std::move(meta.title) : utf8ComposeNfc(stem);
    authorOut = std::move(meta.author);
    return;
  }

  if (!txtmeta::parseTitleAuthorFromStem(stem, titleOut, authorOut)) {
    titleOut = utf8ComposeNfc(stem);
    authorOut.clear();
  }
}

std::string Txt::findCompanionCoverImage(const std::string& filepath) {
  // 1. Check zero-clutter hidden BookCoverStore first (/.crosspoint/covers/<pathHash>.bmp)
  const std::string boundCover = txtmeta::getBoundCoverBmpPath(filepath);
  if (Storage.exists(boundCover.c_str())) {
    return boundCover;
  }

  // 2. Legacy fallback: same-folder companion image (<baseName>.jpg / cover.jpg)
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
                                 bool cropped, bool originalThresholds, int thumbWidth) {
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
  if (thumbHeight > 0) {
    const int targetWidth = thumbWidth > 0 ? thumbWidth : static_cast<int>(thumbHeight * 0.6);
    const int targetHeight = thumbHeight;
    if (isBmp) {
      success = BmpToBmpConverter::bmpFileTo1BitBmpStreamWithSize(src, dst, targetWidth, targetHeight, /*crop=*/true);
    } else if (isJpg) {
      success = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(src, dst, targetWidth, targetHeight);
    } else if (isPng) {
      success = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(src, dst, targetWidth, targetHeight);
    }
  } else {
    if (isBmp) {
      success = BmpToBmpConverter::bmpFileToBmpStream(src, dst, cropped, originalThresholds);
    } else if (isJpg) {
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

void Txt::removeCachedCoverBmps(const std::string& cachePath) {
  if (cachePath.empty() || !Storage.exists(cachePath.c_str())) return;

  HalFile dir = Storage.open(cachePath.c_str());
  if (!dir || !dir.isDirectory()) return;

  std::vector<std::string> toRemove;
  toRemove.reserve(8);
  char nameBuf[128];
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }
    const size_t n = entry.getName(nameBuf, sizeof(nameBuf));
    entry.close();
    if (n == 0) continue;
    const std::string_view name(nameBuf, std::min<size_t>(n, std::strlen(nameBuf)));
    const bool isCoverOrThumbBmp =
        FsHelpers::hasBmpExtension(name) && (name.starts_with("thumb") || name.starts_with("cover"));
    // Remove cover.stamp so Txt::validateCache() detects cover/meta staleness while keeping book.bin's
    // cached byte size intact for source-content modification checks.
    const bool isCoverStamp = (name == "cover.stamp");
    if (isCoverOrThumbBmp || isCoverStamp) {
      toRemove.push_back(cachePath + "/" + std::string(name));
    }
  }
  dir.close();

  for (const auto& file : toRemove) {
    Storage.remove(file.c_str());
  }
}

bool Txt::validateCache(const std::string& filepath, const std::string& cachePath, size_t cachedSize) {
  bool valid = true;
  bool contentChanged = false;

  // 1. If html/0.html exists, check its embedded version comment
  const std::string htmlPath = cachePath + "/html/0.html";
  if (Storage.exists(htmlPath.c_str())) {
    HalFile htmlFile;
    if (Storage.openFileForRead("TXT", htmlPath, htmlFile)) {
      char header[80] = {0};
      const int bytesRead = htmlFile.read(header, sizeof(header) - 1);
      if (bytesRead > 0) {
        header[bytesRead] = '\0';
        if (strstr(header, TxtToHtml::cacheVersionTag(filepath)) == nullptr) {
          LOG_DBG("TXT", "HTML cache version mismatch or missing, invalidating: %s", htmlPath.c_str());
          valid = false;
          contentChanged = true;
        }
      } else {
        valid = false;
        contentChanged = true;
      }
    } else {
      valid = false;
      contentChanged = true;
    }
  }

  // 2. Check if source file size changed
  if (valid && cachedSize > 0) {
    HalFile rawFile;
    if (Storage.openFileForRead("TXT", filepath, rawFile)) {
      if (rawFile.size() != cachedSize) {
        LOG_DBG("TXT", "File size changed (%u vs cached %u), invalidating cache", static_cast<uint32_t>(rawFile.size()),
                static_cast<uint32_t>(cachedSize));
        valid = false;
        contentChanged = true;
      }
    } else {
      valid = false;
      contentChanged = true;
    }
  }

  // 3. Check if a custom/companion cover or bound .meta.json was added, replaced, or removed
  if (valid && !isCoverMetaStampFresh(filepath, cachePath)) {
    LOG_DBG("TXT", "Cover or metadata changed for %s, invalidating cached cover/metadata (keeping progress.bin)",
            filepath.c_str());
    removeCachedCoverBmps(cachePath);
    valid = false;
  }

  if (!valid && contentChanged) {
    LOG_DBG("TXT", "Cache invalid for %s, wiping html and sections", filepath.c_str());
    invalidateCache(cachePath);
  }

  return valid;
}

bool Txt::buildTxtCache(const std::string& filepath, const std::string& cachePath,
                        std::unique_ptr<BookMetadataCache>& bookMetadataCache, const bool preserveContentCache) {
  LOG_DBG("TXT", "Building metadata cache for TXT: %s", filepath.c_str());

  if (!preserveContentCache) {
    invalidateCache(cachePath);
  }
  if (!Storage.exists(cachePath.c_str())) {
    Storage.mkdir(cachePath.c_str());
  }

  bool hasCjk = false;
  const TextEncoding activeEncoding = detectFileEncoding(filepath, cachePath, &hasCjk);

  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("TXT", "Could not begin writing cache");
    return false;
  }

  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("TXT", "Could not begin writing content.opf pass");
    return false;
  }

  bookMetadataCache->createSpineEntry("content.html");

  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("TXT", "Could not end writing content.opf pass");
    return false;
  }

  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("TXT", "Could not begin writing toc pass");
    return false;
  }

  std::string resolvedTitle;
  std::string resolvedAuthor;
  resolveTitleAndAuthor(filepath, resolvedTitle, resolvedAuthor);

  const size_t tocEntriesCreated = txtenc::scanChapterHeadings(filepath, activeEncoding, *bookMetadataCache, hasCjk);
  if (tocEntriesCreated == 0) {
    bookMetadataCache->createTocEntry(resolvedTitle, "content.html", "", 0);
  }

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("TXT", "Could not end writing toc pass");
    return false;
  }

  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("TXT", "Could not end writing cache");
    return false;
  }

  BookMetadataCache::BookMetadata bookMetadata;
  bookMetadata.title = std::move(resolvedTitle);
  bookMetadata.author = std::move(resolvedAuthor);
  bookMetadata.language = hasCjk ? "zh" : "en";

  std::string companionCover = findCompanionCoverImage(filepath);
  if (!companionCover.empty()) {
    bookMetadata.coverItemHref = companionCover;
  }

  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR("TXT", "Could not build book.bin for TXT");
    return false;
  }

  bookMetadataCache->cleanupTmpFiles();
  writeCoverMetaStamp(filepath, cachePath);

  bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  if (!bookMetadataCache || !bookMetadataCache->load()) {
    LOG_ERR("TXT", "Failed to reload cache after build");
    return false;
  }

  return true;
}
