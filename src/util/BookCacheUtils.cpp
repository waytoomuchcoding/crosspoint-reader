#include "BookCacheUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtc.h>

#include "BookCoverStore.h"
#include "ClippingStore.h"

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) {
  if (FsHelpers::hasReflowableBookExtension(path)) {
    Epub(path, "/.crosspoint").clearCache();
  } else if (FsHelpers::hasXtcExtension(path)) {
    Xtc(path, "/.crosspoint").clearCache();
  } else {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

bool removeBookFile(const std::string& path) {
  if (!Storage.remove(path.c_str())) return false;
  clearBookCache(path);
  BookCoverStore::removeBoundAssets(path);
  for (const char* suffix : {".key", ".rights"}) {
    const std::string sidecar = path + suffix;
    if (Storage.exists(sidecar.c_str())) {
      Storage.remove(sidecar.c_str());
    }
  }
  return ClippingStore::deleteForFilePath(path, "epub");
}

void moveBookSidecarState(const std::string& oldPath, const std::string& newPath) {
  BookCoverStore::moveBoundAssets(oldPath, newPath);
  for (const char* suffix : {".key", ".rights"}) {
    const std::string src = oldPath + suffix;
    if (!Storage.exists(src.c_str())) continue;
    const std::string dst = newPath + suffix;
    if (Storage.exists(dst.c_str())) Storage.remove(dst.c_str());
    if (!Storage.rename(src.c_str(), dst.c_str())) {
      LOG_ERR("BookCache", "Failed to move sidecar %s -> %s", src.c_str(), dst.c_str());
    }
  }
}
