#include "ShibaCoverCache.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Memory.h>
#include <Txt.h>
#include <Xtc.h>

#include <functional>

#include "util/BookCoverStore.h"

namespace shibadesk {

namespace {

std::string getXtcCachePath(const std::string& bookPath) {
  return "/.crosspoint/xtc_" + std::to_string(std::hash<std::string>{}(bookPath));
}

std::string formatSizedThumbPath(const std::string& cachePath, const int targetW, const int targetH) {
  return cachePath + "/thumb_" + std::to_string(targetW) + "x" + std::to_string(targetH) + ".bmp";
}

}  // namespace

std::string detectBookFormatTag(const std::string& path) {
  if (FsHelpers::hasMarkdownExtension(path)) return "MD";
  if (FsHelpers::hasTxtExtension(path)) return "TXT";
  if (FsHelpers::hasXtcExtension(path)) return "XTC";
  return "EPUB";
}

std::string lookupExistingThumbBmp(const std::string& bookPath, const int targetW, const int targetH) {
  if (bookPath.empty() || targetW <= 0 || targetH <= 0) return {};

  if (FsHelpers::hasReflowableBookExtension(bookPath)) {
    const std::string cachePath = Txt::getBookCachePath(bookPath);
    const std::string exact = formatSizedThumbPath(cachePath, targetW, targetH);
    if (Storage.exists(exact.c_str())) return exact;
    return {};
  }

  if (FsHelpers::hasXtcExtension(bookPath)) {
    const std::string exact = getXtcCachePath(bookPath) + "/thumb_" + std::to_string(targetH) + ".bmp";
    if (Storage.exists(exact.c_str())) return exact;
  }

  return {};
}

std::string generateThumbBmp(const std::string& bookPath, const int targetW, const int targetH,
                             const std::function<void()>& beforeHeavyIo, const std::function<void()>& afterHeavyIo) {
  const std::string existing = lookupExistingThumbBmp(bookPath, targetW, targetH);
  if (!existing.empty()) return existing;

  if (Txt::isTxtOrMd(bookPath)) {
    const std::string srcImg = Txt::findCompanionCoverImage(bookPath);
    if (srcImg.empty()) return {};

    const std::string cachePath = Txt::getBookCachePath(bookPath);
    const std::string dest = formatSizedThumbPath(cachePath, targetW, targetH);
    if (beforeHeavyIo) beforeHeavyIo();
    Storage.ensureDirectoryExists("/.crosspoint");
    Storage.ensureDirectoryExists(cachePath.c_str());
    const bool ok = BookCoverStore::convertImageFileTo1BitBmp(srcImg, dest, targetW, targetH);
    if (afterHeavyIo) afterHeavyIo();
    return ok ? dest : std::string();
  }

  if (FsHelpers::hasEpubExtension(bookPath)) {
    auto epub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
    if (!epub) return {};
    const std::string dest = epub->getThumbBmpPath(targetW, targetH);
    if (beforeHeavyIo) beforeHeavyIo();
    const bool ok = epub->generateThumbBmpFromSourceWithSize(targetW, targetH);
    if (afterHeavyIo) afterHeavyIo();
    return ok ? dest : std::string();
  }

  if (FsHelpers::hasXtcExtension(bookPath)) {
    auto xtc = makeUniqueNoThrow<Xtc>(bookPath, "/.crosspoint");
    if (!xtc) return {};
    const std::string dest = xtc->getThumbBmpPath(targetH);
    if (beforeHeavyIo) beforeHeavyIo();
    const bool ok = xtc->load() && xtc->generateThumbBmp(targetH);
    if (afterHeavyIo) afterHeavyIo();
    return ok ? dest : std::string();
  }

  return {};
}

bool drawThumbBmp(GfxRenderer& renderer, const std::string& bmpPath, const int x, const int y, const int w,
                  const int h) {
  if (bmpPath.empty() || w <= 0 || h <= 0) return false;
  auto bmpFile = makeUniqueNoThrow<HalFile>();
  if (!bmpFile || !Storage.openFileForRead("SHBA", bmpPath, *bmpFile)) {
    return false;
  }
  auto bmp = makeUniqueNoThrow<Bitmap>(*bmpFile);
  bool painted = false;
  if (bmp && bmp->parseHeaders() == BmpReaderError::Ok) {
    renderer.fillRect(x, y, w, h, false);
    const int bw = bmp->getWidth();
    const int bh = bmp->getHeight();
    const int drawX = (bw > 0 && bw < w) ? (x + (w - bw) / 2) : x;
    const int drawY = (bh > 0 && bh < h) ? (y + (h - bh) / 2) : y;
    painted = renderer.drawBitmap(*bmp, drawX, drawY, w, h);
  }
  bmp.reset();
  bmpFile->close();
  return painted;
}

}  // namespace shibadesk
