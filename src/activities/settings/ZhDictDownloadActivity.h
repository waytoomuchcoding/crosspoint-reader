#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

// Settings > Reader > Download Chinese Dictionary: joins Wi-Fi, reads the
// manifest of the fork's `zh-dict` release (scripts/zh/package_release.py),
// and installs the dictionary into /zh and any fonts into /.fonts. The new
// dictionary is downloaded into /zh.new and swapped in only once every file
// has arrived with the right size and CRC32, so a failed download leaves the
// old one working. The device restarts afterwards to release Wi-Fi memory and
// pick up new fonts.
class ZhDictDownloadActivity final : public Activity {
 public:
  ZhDictDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ZhDictDownload", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::Downloading; }

 private:
  enum class State : uint8_t { Wifi, Checking, Confirm, Downloading, Done, Failed };
  struct File {
    std::string asset;
    std::string path;
    uint32_t size = 0;
    uint32_t crc32 = 0;
  };

  void onWifi(bool connected);
  bool fetchManifest();
  void download();
  bool installFile(const File& file, size_t index);
  bool swapInDictionary();

  State state = State::Wifi;
  std::string version;
  std::string installedVersion;
  std::vector<File> files;
  uint32_t totalBytes = 0;
  size_t currentFile = 0;
  uint32_t doneBytes = 0;
  uint32_t fileBytes = 0;
  std::string error;
};
