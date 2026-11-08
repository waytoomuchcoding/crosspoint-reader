#include "ZhDictDownloadActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <ZhDict.h>
#include <esp_rom_crc.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

#ifndef ZH_DICT_RELEASE_URL
// The fork's release built by .github/workflows/zh-dictionary.yml.
#define ZH_DICT_RELEASE_URL "https://github.com/waytoomuchcoding/crosspoint-reader/releases/download/zh-dict/"
#endif

namespace {

constexpr char DICT_DIR[] = "/zh";
constexpr char NEW_DIR[] = "/zh.new";
constexpr char OLD_DIR[] = "/zh.old";
constexpr char VERSION_FILE[] = "/zh/version.txt";

bool crc32Of(const char* path, uint32_t& out) {
  HalFile f;
  if (!Storage.openFileForRead("ZHDL", path, f)) return false;
  uint8_t buf[512];
  uint32_t crc = 0;
  int n;
  while ((n = f.read(buf, sizeof(buf))) > 0) crc = esp_rom_crc32_le(crc, buf, static_cast<uint32_t>(n));
  out = crc;
  return true;
}

// Ensures every directory above `path` exists.
void makeParents(const std::string& path) {
  const size_t slash = path.rfind('/');
  if (slash != std::string::npos && slash > 0) Storage.mkdir(path.substr(0, slash).c_str());
}

}  // namespace

void ZhDictDownloadActivity::onEnter() {
  Activity::onEnter();
  char buf[32] = {};
  if (Storage.exists(VERSION_FILE) && Storage.readFileToBuffer(VERSION_FILE, buf, sizeof(buf)) > 0) {
    installedVersion = buf;
    while (!installedVersion.empty() && (installedVersion.back() == '\n' || installedVersion.back() == '\r')) {
      installedVersion.pop_back();
    }
  }
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifi(!result.isCancelled); });
}

void ZhDictDownloadActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    // Wi-Fi fragments the heap on the C3; restart, as Manage Fonts does.
    silentRestart();
  }
}

void ZhDictDownloadActivity::onWifi(const bool connected) {
  if (!connected) {
    finish();
    return;
  }
  {
    RenderLock lock(*this);
    state = State::Checking;
  }
  requestUpdateAndWait();
  const bool ok = fetchManifest();
  {
    RenderLock lock(*this);
    state = ok ? State::Confirm : State::Failed;
  }
  requestUpdate();
}

bool ZhDictDownloadActivity::fetchManifest() {
  std::string body;
  body.reserve(4096);
  const bool fetched = HttpDownloader::fetchUrl(std::string(ZH_DICT_RELEASE_URL) + "manifest.json",
                                                [&](const uint8_t* data, size_t len) {
                                                  if (body.size() + len > 32768) return false;  // a manifest is ~1 KB
                                                  body.append(reinterpret_cast<const char*>(data), len);
                                                  return true;
                                                });
  if (!fetched) {
    error = "manifest: HTTP " + std::to_string(HttpDownloader::lastStatus());
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body) || doc["format"].as<int>() != 1) {
    error = "manifest: unreadable";
    return false;
  }
  version = doc["version"].as<const char*>() ? doc["version"].as<const char*>() : "";
  files.clear();
  totalBytes = 0;
  for (JsonObject f : doc["files"].as<JsonArray>()) {
    File file;
    file.asset = f["asset"] | "";
    file.path = f["path"] | "";
    file.size = f["size"] | 0u;
    file.crc32 = f["crc32"] | 0u;
    // Only the two data folders may be written.
    const bool allowed = file.path.rfind("zh/", 0) == 0 || file.path.rfind(".fonts/", 0) == 0;
    if (file.asset.empty() || !allowed || file.path.find("..") != std::string::npos) {
      error = "manifest: bad entry " + file.path;
      return false;
    }
    totalBytes += file.size;
    files.push_back(std::move(file));
  }
  LOG_INF("ZHDL", "manifest %s: %u files, %u bytes", version.c_str(), static_cast<unsigned>(files.size()),
          static_cast<unsigned>(totalBytes));
  return !files.empty();
}

bool ZhDictDownloadActivity::installFile(const File& file, const size_t index) {
  // Dictionary files stage in /zh.new; fonts are independent and go in place.
  std::string dest = "/" + file.path;
  if (file.path.rfind("zh/", 0) == 0) dest = std::string(NEW_DIR) + "/" + file.path.substr(3);
  makeParents(dest);
  {
    RenderLock lock(*this);
    currentFile = index;
    fileBytes = 0;
  }
  requestUpdate();
  unsigned long lastDraw = 0;
  const auto err = HttpDownloader::downloadToFile(
      std::string(ZH_DICT_RELEASE_URL) + file.asset, dest,
      [&](size_t downloaded, size_t) {
        fileBytes = static_cast<uint32_t>(downloaded);
        if (millis() - lastDraw > 2000) {
          lastDraw = millis();
          requestUpdate();
        }
      },
      nullptr, "", "", {}, true);
  if (err != HttpDownloader::OK) {
    error = file.asset + ": download failed (" + std::to_string(HttpDownloader::lastStatus()) + ")";
    return false;
  }
  uint32_t crc = 0;
  HalFile f;
  const size_t size = Storage.openFileForRead("ZHDL", dest.c_str(), f) ? f.fileSize() : 0;
  f.close();
  if (size != file.size || !crc32Of(dest.c_str(), crc) || crc != file.crc32) {
    error = file.asset + ": size or CRC mismatch";
    Storage.remove(dest.c_str());
    return false;
  }
  doneBytes += file.size;
  return true;
}

bool ZhDictDownloadActivity::swapInDictionary() {
  zh::resetSharedDict();  // close any open handles to /zh
  if (Storage.exists(OLD_DIR)) Storage.removeDir(OLD_DIR);
  if (Storage.exists(DICT_DIR) && !Storage.rename(DICT_DIR, OLD_DIR)) return false;
  if (!Storage.rename(NEW_DIR, DICT_DIR)) {
    Storage.rename(OLD_DIR, DICT_DIR);  // put the old one back
    return false;
  }
  Storage.removeDir(OLD_DIR);
  if (HalFile v = Storage.open(VERSION_FILE, O_WRITE | O_CREAT | O_TRUNC)) {
    v.write(version.data(), version.size());
    v.close();
  }
  zh::resetSharedDict();
  return true;
}

void ZhDictDownloadActivity::download() {
  const auto heap = HalMemory::getDefaultHeap();
  if (heap.freeBytes < HttpDownloader::MIN_TLS_FREE_HEAP) {
    error = "not enough memory for TLS; restart and try again";
    RenderLock lock(*this);
    state = State::Failed;
    return;
  }
  if (Storage.exists(NEW_DIR)) Storage.removeDir(NEW_DIR);
  Storage.mkdir(NEW_DIR);
  doneBytes = 0;
  bool ok = true;
  for (size_t i = 0; i < files.size() && ok; ++i) ok = installFile(files[i], i);
  if (ok && !swapInDictionary()) {
    error = "could not replace /zh";
    ok = false;
  }
  if (!ok) Storage.removeDir(NEW_DIR);
  RenderLock lock(*this);
  state = ok ? State::Done : State::Failed;
}

void ZhDictDownloadActivity::loop() {
  using B = MappedInputManager::Button;
  switch (state) {
    case State::Confirm:
      if (mappedInput.wasReleased(B::Confirm)) {
        {
          RenderLock lock(*this);
          state = State::Downloading;
        }
        requestUpdateAndWait();
        download();
        requestUpdate();
      } else if (mappedInput.wasReleased(B::Back)) {
        finish();
      }
      break;
    case State::Done:
    case State::Failed:
      if (mappedInput.wasReleased(B::Confirm) || mappedInput.wasReleased(B::Back)) finish();
      break;
    default:
      break;
  }
}

void ZhDictDownloadActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_ZH_DICT_DOWNLOAD));
  const int x = metrics.contentSidePadding;
  const int contentWidth = width - 2 * x;
  const int lh = renderer.getLineHeight(UI_10_FONT_ID);
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 3;
  const auto para = [&](const std::string& text) {
    for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, text.c_str(), contentWidth, 8)) {
      renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
      y += lh;
    }
    y += metrics.verticalSpacing;
  };
  char buf[96];
  const char* confirm = "";
  switch (state) {
    case State::Wifi:
    case State::Checking:
      para(tr(STR_ZH_DICT_CHECKING));
      break;
    case State::Confirm:
      snprintf(buf, sizeof(buf), "%s: %s", tr(STR_ZH_DICT_INSTALLED),
               installedVersion.empty() ? tr(STR_ZH_DICT_NONE) : installedVersion.c_str());
      para(buf);
      snprintf(buf, sizeof(buf), "%s: %s (%u MB, %u files)", tr(STR_ZH_DICT_AVAILABLE), version.c_str(),
               static_cast<unsigned>((totalBytes + 500000) / 1000000), static_cast<unsigned>(files.size()));
      para(buf);
      para(tr(STR_ZH_DICT_PROMPT));
      confirm = tr(STR_ZH_DICT_DOWNLOAD_SHORT);
      break;
    case State::Downloading: {
      const uint32_t done = doneBytes + fileBytes;
      const int pct = totalBytes ? static_cast<int>(static_cast<uint64_t>(done) * 100 / totalBytes) : 0;
      snprintf(buf, sizeof(buf), "%s %u/%u: %s", tr(STR_ZH_DICT_DOWNLOADING), static_cast<unsigned>(currentFile + 1),
               static_cast<unsigned>(files.size()), currentFile < files.size() ? files[currentFile].asset.c_str() : "");
      para(buf);
      // Progress bar
      renderer.drawRect(x, y, contentWidth, 18, true);
      renderer.fillRect(x + 2, y + 2, (contentWidth - 4) * pct / 100, 14, true);
      y += 18 + metrics.verticalSpacing;
      snprintf(buf, sizeof(buf), "%d%%  (%u / %u MB)", pct, static_cast<unsigned>(done / 1000000),
               static_cast<unsigned>(totalBytes / 1000000));
      para(buf);
      break;
    }
    case State::Done:
      para(tr(STR_ZH_DICT_DONE));
      confirm = tr(STR_DONE);
      break;
    case State::Failed:
      para(tr(STR_ZH_DICT_FAILED));
      para(error);
      confirm = tr(STR_DONE);
      break;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
