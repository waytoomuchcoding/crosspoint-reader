#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

class TelegramCredentialStore;

namespace telegram {

struct PendingFileUpdate {
  int64_t updateId = 0;
  int64_t messageId = 0;
  uint32_t messageDate = 0;
  bool isCoverImage = false;
  std::string fileId;
  std::string fileName;
  size_t fileSize = 0;
  std::string caption;
  int64_t replyToMessageId = 0;
  std::string replyToFileName;
};

struct SyncSummary {
  size_t updatesProcessed = 0;
  size_t filesDownloaded = 0;
  size_t coversBound = 0;
  std::vector<std::string> savedPaths;
};

using ProgressCallback = std::function<void(const std::string& currentFile, size_t downloaded, size_t total)>;

// Validates supported extensions: .epub, .txt, .md, .xtc, .acsm, .cpfont
bool isSupportedBookExtension(std::string_view filename);
// Validates supported cover image extensions: .jpg, .jpeg, .png, .bmp
bool isSupportedImageExtension(std::string_view filename);

// Extracts the SD font family folder name from a `.cpfont` filename (e.g. `NotoSansSC_14.cpfont` -> `NotoSansSC`,
// `LXGW_WenKai_16_bold.cpfont` -> `LXGW_WenKai`). Returns empty string for invalid names.
std::string extractCpfontFamilyName(std::string_view filename);

// Resolves `/fonts/<Family>/<filename>` (or `/.fonts/<Family>/<filename>` when `/.fonts` is the active root)
// so `SdCardFontRegistry::discover()` indexes Telegram-delivered `.cpfont` files immediately.
std::string resolveCpfontDestPath(std::string_view filename);

// Redacts `/bot<token>` and raw `<token>` occurrences from diagnostic strings so tokens never enter serial logs.
std::string redactBotToken(std::string_view text, std::string_view botToken);

// Resolves which local .txt/.md book an incoming Telegram cover photo should bind to, in strict priority:
// 1. Explicit reply_to_message (ring buffer message_id or reply_to_message.document.file_name in destFolder)
// 2. Photo caption stem match against an existing .txt/.md in destFolder
// 3. Sequential / split-sync fallback to lastSyncedTxtPath_ (within 1-hour server timestamp window)
// Returns empty string when no target .txt/.md matches (caller saves image cleanly into /covers/).
std::string resolveCoverTargetTxtPath(const PendingFileUpdate& upd, const TelegramCredentialStore& store);

// Pure JSON helpers (unit-testable without network)
bool parsePairingChatFromUpdatesJson(std::string_view updatesJson, std::string& chatIdOut, std::string& usernameOut,
                                     std::string& errOut);

bool parseFileUpdatesJson(std::string_view updatesJson, std::string_view allowedChatId,
                          std::vector<PendingFileUpdate>& updatesOut, int64_t& highestUpdateIdOut, std::string& errOut);

bool parseGetFilePathJson(std::string_view getFileJson, std::string& filePathOut, std::string& errOut);

// Network operations against api.telegram.org
bool pairLatestChat(std::string_view botToken, std::string& chatIdOut, std::string& usernameOut, std::string& errOut);

// shouldStop (optional): checked before each update batch; true ends the sync.
bool syncPendingFiles(SyncSummary& summaryOut, std::string& errOut, ProgressCallback progressCb = nullptr,
                      const std::function<bool()>& shouldStop = nullptr);

}  // namespace telegram
