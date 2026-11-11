#include "BookCoverStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <Txt.h>

#include "RecentBooksStore.h"

namespace {

uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t readLe16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8); }

uint32_t currentMetadataTimestamp() {
  const int64_t now = trustedtime::trustedNow();
  if (now > 0) return static_cast<uint32_t>(now);
  return static_cast<uint32_t>(millis() / 1000u + 1u);
}

bool ensureCoversDirectory() {
  if (!Storage.exists("/.crosspoint")) {
    Storage.mkdir("/.crosspoint");
  }
  if (!Storage.exists(txtmeta::kHiddenCoversDir)) {
    Storage.mkdir(txtmeta::kHiddenCoversDir);
  }
  return Storage.exists(txtmeta::kHiddenCoversDir);
}

bool updateMetaFields(const std::string& bookPath, const std::string* optionalTitle,
                      const std::string* optionalAuthor) {
  if (!optionalTitle && !optionalAuthor) return true;

  const std::string metaPath = txtmeta::getBoundMetaJsonPath(bookPath);
  txtmeta::BoundBookMeta meta;
  txtmeta::readMetaJson(metaPath, meta);
  if (optionalTitle) meta.title = *optionalTitle;
  if (optionalAuthor) meta.author = *optionalAuthor;
  meta.updatedAt = currentMetadataTimestamp();

  if (meta.title.empty() && meta.author.empty()) {
    if (Storage.exists(metaPath.c_str())) {
      Storage.remove(metaPath.c_str());
    }
    return true;
  }
  return txtmeta::writeMetaJson(metaPath, meta);
}

void moveOptionalAsset(const std::string& oldAsset, const std::string& newAsset) {
  if (oldAsset.empty() || newAsset.empty() || oldAsset == newAsset) return;
  if (!Storage.exists(oldAsset.c_str())) return;
  if (Storage.exists(newAsset.c_str())) {
    Storage.remove(newAsset.c_str());
  }
  if (!Storage.rename(oldAsset.c_str(), newAsset.c_str())) {
    LOG_ERR("COV", "Failed moving bound asset %s -> %s", oldAsset.c_str(), newAsset.c_str());
  }
}

}  // namespace

std::string BookCoverStore::getBoundCoverBmpPath(std::string_view bookPath) {
  return txtmeta::getBoundCoverBmpPath(bookPath);
}

std::string BookCoverStore::getBoundMetaJsonPath(std::string_view bookPath) {
  return txtmeta::getBoundMetaJsonPath(bookPath);
}

bool BookCoverStore::hasBoundCover(std::string_view bookPath) {
  const std::string path = txtmeta::getBoundCoverBmpPath(bookPath);
  return Storage.exists(path.c_str());
}

bool BookCoverStore::readBoundMeta(std::string_view bookPath, txtmeta::BoundBookMeta& out) {
  return txtmeta::readMetaJson(txtmeta::getBoundMetaJsonPath(bookPath), out);
}

bool BookCoverStore::validate1BitBmpPayload(const uint8_t* data, const size_t len, int* widthOut, int* heightOut) {
  if (!data || len < 62 || len > 65536) return false;
  if (data[0] != 'B' || data[1] != 'M') return false;
  const uint32_t declaredSize = readLe32(data + 2);
  const uint32_t pixelOffset = readLe32(data + 10);
  const uint32_t dibSize = readLe32(data + 14);
  if (dibSize < 40 || pixelOffset < 54 || pixelOffset > len || declaredSize > len) return false;

  const auto rawW = static_cast<int32_t>(readLe32(data + 18));
  const auto rawH = static_cast<int32_t>(readLe32(data + 22));
  const uint16_t planes = readLe16(data + 26);
  const uint16_t bpp = readLe16(data + 28);
  const uint32_t compression = readLe32(data + 30);
  if (planes != 1 || bpp != 1 || compression != 0) return false;

  const int absH = rawH < 0 ? -rawH : rawH;
  if (rawW <= 0 || rawW > 800 || absH <= 0 || absH > 1200) return false;
  const size_t rowBytes = (static_cast<size_t>(rawW) + 31u) / 32u * 4u;
  if (pixelOffset + rowBytes * static_cast<size_t>(absH) > len) return false;

  if (widthOut) *widthOut = rawW;
  if (heightOut) *heightOut = absH;
  return true;
}

bool BookCoverStore::convertImageFileTo1BitBmp(const std::string& sourceImagePath, const std::string& destBmpPath,
                                               const int targetWidth, const int targetHeight) {
  if (sourceImagePath.empty() || destBmpPath.empty() || targetWidth <= 0 || targetHeight <= 0) return false;
  return Txt::convertCoverImageToBmp(sourceImagePath, destBmpPath, targetHeight, /*cropped=*/true,
                                     /*originalThresholds=*/false, targetWidth);
}

bool BookCoverStore::bindCoverAndMetaFromImageFile(const std::string& bookPath, const std::string& sourceImagePath,
                                                   const std::string* optionalTitle,
                                                   const std::string* optionalAuthor) {
  if (bookPath.empty() || !ensureCoversDirectory()) return false;

  const std::string destCoverPath = txtmeta::getBoundCoverBmpPath(bookPath);
  if (!convertImageFileTo1BitBmp(sourceImagePath, destCoverPath, kCanonicalCoverWidth, kCanonicalCoverHeight)) {
    LOG_ERR("COV", "Failed to convert cover image %s for %s", sourceImagePath.c_str(), bookPath.c_str());
    return false;
  }

  if (!updateMetaFields(bookPath, optionalTitle, optionalAuthor)) {
    LOG_ERR("COV", "Failed to update metadata JSON for %s", bookPath.c_str());
    return false;
  }

  refreshBookStateAfterCoverOrMetaChange(bookPath);
  return true;
}

bool BookCoverStore::bindCoverAndMetaFrom1BitBmpBytes(const std::string& bookPath, const uint8_t* bmpData,
                                                      const size_t bmpLen, const std::string* optionalTitle,
                                                      const std::string* optionalAuthor) {
  if (bookPath.empty() || !ensureCoversDirectory()) return false;

  int bmpW = 0;
  int bmpH = 0;
  if (!validate1BitBmpPayload(bmpData, bmpLen, &bmpW, &bmpH)) {
    LOG_ERR("COV", "Rejected invalid 1-bit BMP payload (%zu bytes) for %s", bmpLen, bookPath.c_str());
    return false;
  }

  const std::string destCoverPath = txtmeta::getBoundCoverBmpPath(bookPath);
  const std::string rawTmpPath = destCoverPath + ".raw.tmp";
  HalFile f;
  if (!Storage.openFileForWrite("COV", rawTmpPath, f)) {
    return false;
  }
  const bool written = (f.write(bmpData, bmpLen) == bmpLen);
  const bool closed = f.close();
  if (!written || !closed) {
    Storage.remove(rawTmpPath.c_str());
    return false;
  }

  bool placed = false;
  if (bmpW == kCanonicalCoverWidth && bmpH == kCanonicalCoverHeight) {
    if (Storage.exists(destCoverPath.c_str())) {
      Storage.remove(destCoverPath.c_str());
    }
    placed = Storage.rename(rawTmpPath.c_str(), destCoverPath.c_str());
  } else {
    placed = convertImageFileTo1BitBmp(rawTmpPath, destCoverPath, kCanonicalCoverWidth, kCanonicalCoverHeight);
  }
  Storage.remove(rawTmpPath.c_str());
  if (!placed) {
    LOG_ERR("COV", "Failed to store 1-bit BMP cover for %s", bookPath.c_str());
    return false;
  }

  if (!updateMetaFields(bookPath, optionalTitle, optionalAuthor)) {
    LOG_ERR("COV", "Failed to update metadata JSON for %s", bookPath.c_str());
    return false;
  }

  refreshBookStateAfterCoverOrMetaChange(bookPath);
  return true;
}

bool BookCoverStore::bindMetadataOnly(const std::string& bookPath, const std::string& title,
                                      const std::string& author) {
  if (bookPath.empty() || !ensureCoversDirectory()) return false;
  if (!updateMetaFields(bookPath, &title, &author)) {
    return false;
  }
  refreshBookStateAfterCoverOrMetaChange(bookPath);
  return true;
}

bool BookCoverStore::clearBoundCover(const std::string& bookPath) {
  if (bookPath.empty()) return false;
  const std::string coverPath = txtmeta::getBoundCoverBmpPath(bookPath);
  if (Storage.exists(coverPath.c_str())) {
    Storage.remove(coverPath.c_str());
  }
  refreshBookStateAfterCoverOrMetaChange(bookPath);
  return true;
}

void BookCoverStore::removeBoundAssets(const std::string& bookPath) {
  if (bookPath.empty()) return;
  const std::string coverPath = txtmeta::getBoundCoverBmpPath(bookPath);
  if (Storage.exists(coverPath.c_str())) {
    Storage.remove(coverPath.c_str());
  }
  const std::string metaPath = txtmeta::getBoundMetaJsonPath(bookPath);
  if (Storage.exists(metaPath.c_str())) {
    Storage.remove(metaPath.c_str());
  }
  const std::string loanMetaPath = bookPath + ".meta.json";
  if (Storage.exists(loanMetaPath.c_str())) {
    Storage.remove(loanMetaPath.c_str());
  }
}

void BookCoverStore::moveBoundAssets(const std::string& oldBookPath, const std::string& newBookPath) {
  if (oldBookPath.empty() || newBookPath.empty() || oldBookPath == newBookPath) return;
  moveOptionalAsset(txtmeta::getBoundCoverBmpPath(oldBookPath), txtmeta::getBoundCoverBmpPath(newBookPath));
  moveOptionalAsset(txtmeta::getBoundMetaJsonPath(oldBookPath), txtmeta::getBoundMetaJsonPath(newBookPath));
  moveOptionalAsset(oldBookPath + ".meta.json", newBookPath + ".meta.json");
}

void BookCoverStore::refreshBookStateAfterCoverOrMetaChange(const std::string& bookPath) {
  if (bookPath.empty()) return;

  const std::string cachePath = Txt::getBookCachePath(bookPath);
  Txt::removeCachedCoverBmps(cachePath);

  if (Txt::isTxtOrMd(bookPath)) {
    std::string resolvedTitle;
    std::string resolvedAuthor;
    Txt::resolveTitleAndAuthor(bookPath, resolvedTitle, resolvedAuthor);
    const std::string companionCover = Txt::findCompanionCoverImage(bookPath);
    const std::string thumbTemplate = companionCover.empty() ? std::string() : (cachePath + "/thumb_[HEIGHT].bmp");
    RECENT_BOOKS.updateBook(bookPath, resolvedTitle, resolvedAuthor, thumbTemplate);
  }

  library::markLibraryIndexDirty();
}
