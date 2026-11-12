#include "TelegramBookSync.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFontRegistry.h>
#include <SecureHttpClient.h>
#include <TxtCoverMeta.h>
#include <strings.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <memory>

#include "SdCardFontSystem.h"
#include "TelegramCredentialStore.h"
#include "network/AdeptFulfiller.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/BookCoverStore.h"
#include "util/PluginHttp.h"
#include "util/StringUtils.h"

namespace telegram {

namespace {

constexpr char kTelegramApiBase[] = "https://api.telegram.org";
constexpr size_t kMaxTelegramFileBytes = 20 * 1024 * 1024;  // Telegram Bot API getFile limit = 20 MB
constexpr uint32_t kSplitSyncWindowSec = 3600;              // 1-hour split-sync cover pairing window

bool hasSuffixIgnoreCase(std::string_view str, std::string_view suffix) {
  if (str.size() < suffix.size()) return false;
  return strncasecmp(str.data() + (str.size() - suffix.size()), suffix.data(), suffix.size()) == 0;
}

bool isValidFontIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; }

bool endsWithDigitsAfterUnderscore(std::string_view stem, size_t& underscorePosOut) {
  const size_t us = stem.rfind('_');
  if (us == std::string_view::npos || us == 0 || us + 1 >= stem.size()) return false;
  for (size_t i = us + 1; i < stem.size(); ++i) {
    if (stem[i] < '0' || stem[i] > '9') return false;
  }
  underscorePosOut = us;
  return true;
}

void sendConfirmationReply(freeink::SecureHttpClient* session, std::string_view botToken, std::string_view chatId,
                           const std::string& messageText) {
  if (botToken.empty() || chatId.empty()) return;
  JsonDocument bodyDoc;
  bodyDoc["chat_id"] = std::string(chatId);
  bodyDoc["text"] = messageText;
  std::string body;
  serializeJson(bodyDoc, body);

  pluginhttp::Headers headers = {
      {"Content-Type", "application/json"},
      {"Accept", "application/json"},
  };
  String resp;
  const std::string url = std::string(kTelegramApiBase) + "/bot" + std::string(botToken) + "/sendMessage";
  pluginhttp::request(session, url, "POST", body, headers, resp, 4 * 1024);
}

std::string_view trimCaptionSpaces(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
    s.remove_suffix(1);
  }
  return s;
}

std::string findExistingTxtInFolderByStem(const std::string& folder, std::string_view rawStem) {
  const std::string_view trimmed = trimCaptionSpaces(rawStem);
  if (trimmed.empty()) return {};

  if (txtmeta::isTxtOrMdPath(trimmed)) {
    const std::string san = StringUtils::sanitizeFilenamePreservingExtension(std::string(trimmed));
    if (!san.empty()) {
      const std::string direct = folder + "/" + san;
      if (Storage.exists(direct.c_str())) return direct;
    }
  }

  static constexpr const char* kTxtExts[] = {".txt", ".TXT", ".md", ".MD"};
  for (const char* ext : kTxtExts) {
    const std::string san = StringUtils::sanitizeFilenamePreservingExtension(std::string(trimmed) + ext);
    if (san.empty()) continue;
    const std::string candidate = folder + "/" + san;
    if (Storage.exists(candidate.c_str())) {
      return candidate;
    }
  }
  return {};
}

const char* imageExtFromMime(const char* mime) {
  if (!mime || mime[0] == '\0') return nullptr;
  if (strcasecmp(mime, "image/jpeg") == 0 || strcasecmp(mime, "image/jpg") == 0) return ".jpg";
  if (strcasecmp(mime, "image/png") == 0) return ".png";
  if (strcasecmp(mime, "image/bmp") == 0 || strcasecmp(mime, "image/x-ms-bmp") == 0) return ".bmp";
  return nullptr;
}

}  // namespace

bool isSupportedBookExtension(std::string_view filename) {
  static constexpr std::string_view kExts[] = {".epub", ".txt", ".md", ".xtc", ".acsm", ".cpfont"};
  for (std::string_view ext : kExts) {
    if (hasSuffixIgnoreCase(filename, ext)) {
      return true;
    }
  }
  return false;
}

bool isSupportedImageExtension(std::string_view filename) {
  static constexpr std::string_view kImgExts[] = {".jpg", ".jpeg", ".png", ".bmp"};
  for (std::string_view ext : kImgExts) {
    if (hasSuffixIgnoreCase(filename, ext)) {
      return true;
    }
  }
  return false;
}

std::string extractCpfontFamilyName(std::string_view filename) {
  if (!hasSuffixIgnoreCase(filename, ".cpfont")) return {};
  std::string_view stem = filename.substr(0, filename.size() - 7);
  if (stem.empty()) return {};
  for (char c : stem) {
    if (!isValidFontIdentChar(c)) return {};
  }

  // Strip optional v3 style suffix (`_bold_italic`, `_regular`, `_bold`, `_italic`) before `_<size>`.
  static constexpr std::string_view kStyleSuffixes[] = {"_bold_italic", "_regular", "_bold", "_italic"};
  std::string_view trimmed = stem;
  for (std::string_view suf : kStyleSuffixes) {
    if (hasSuffixIgnoreCase(trimmed, suf) && trimmed.size() > suf.size()) {
      const std::string_view beforeStyle = trimmed.substr(0, trimmed.size() - suf.size());
      size_t sizeUs = 0;
      if (endsWithDigitsAfterUnderscore(beforeStyle, sizeUs)) {
        return std::string(beforeStyle.substr(0, sizeUs));
      }
    }
  }

  size_t sizeUs = 0;
  if (endsWithDigitsAfterUnderscore(trimmed, sizeUs)) {
    return std::string(trimmed.substr(0, sizeUs));
  }
  return std::string(stem);
}

std::string resolveCpfontDestPath(std::string_view filename) {
  const std::string family = extractCpfontFamilyName(filename);
  if (family.empty()) return {};

  const std::string hiddenFamilyDir = std::string(SdCardFontRegistry::FONTS_DIR_HIDDEN) + "/" + family;
  const std::string visibleFamilyDir = std::string(SdCardFontRegistry::FONTS_DIR_VISIBLE) + "/" + family;

  const char* root = SdCardFontRegistry::FONTS_DIR_VISIBLE;
  if (Storage.exists(hiddenFamilyDir.c_str())) {
    root = SdCardFontRegistry::FONTS_DIR_HIDDEN;
  } else if (Storage.exists(visibleFamilyDir.c_str())) {
    root = SdCardFontRegistry::FONTS_DIR_VISIBLE;
  } else if (Storage.exists(SdCardFontRegistry::FONTS_DIR_HIDDEN) &&
             !Storage.exists(SdCardFontRegistry::FONTS_DIR_VISIBLE)) {
    root = SdCardFontRegistry::FONTS_DIR_HIDDEN;
  }

  return std::string(root) + "/" + family + "/" + std::string(filename);
}

std::string redactBotToken(std::string_view text, std::string_view botToken) {
  std::string out(text);
  if (botToken.empty()) return out;
  size_t pos = 0;
  while ((pos = out.find(botToken, pos)) != std::string::npos) {
    out.replace(pos, botToken.size(), "<redacted>");
    pos += 10;
  }
  return out;
}

std::string resolveCoverTargetTxtPath(const PendingFileUpdate& upd, const TelegramCredentialStore& store) {
  const std::string& destFolder = store.getDestFolder();

  // 1. Explicit reply_to_message (ring buffer message_id OR reply_to_message.document.file_name in destFolder)
  if (upd.replyToMessageId > 0) {
    const std::string boundPath = store.findBookPathByMessageId(upd.replyToMessageId);
    if (!boundPath.empty() && txtmeta::isTxtOrMdPath(boundPath) && Storage.exists(boundPath.c_str())) {
      return boundPath;
    }
  }
  if (!upd.replyToFileName.empty()) {
    const std::string sanReply = StringUtils::sanitizeFilenamePreservingExtension(upd.replyToFileName);
    if (!sanReply.empty() && txtmeta::isTxtOrMdPath(sanReply)) {
      const std::string replyCandidate = destFolder + "/" + sanReply;
      if (Storage.exists(replyCandidate.c_str())) {
        return replyCandidate;
      }
    }
  }

  // 2. Caption stem match against an existing .txt/.md in destFolder
  if (!upd.caption.empty()) {
    std::string matched = findExistingTxtInFolderByStem(destFolder, upd.caption);
    if (!matched.empty()) {
      return matched;
    }
    std::string capTitle;
    std::string capAuthor;
    if (txtmeta::parseTitleAuthorFromStem(upd.caption, capTitle, capAuthor)) {
      matched = findExistingTxtInFolderByStem(destFolder, capTitle);
      if (!matched.empty()) {
        return matched;
      }
    }
  }

  // 3. Sequential / split-sync fallback to lastSyncedTxtPath_ within the 1-hour server timestamp window
  const std::string& lastTxt = store.getLastSyncedTxtPath();
  if (!lastTxt.empty() && txtmeta::isTxtOrMdPath(lastTxt) && Storage.exists(lastTxt.c_str())) {
    const uint32_t lastTs = store.getLastSyncedTxtTimestamp();
    if (upd.messageDate > 0 && lastTs > 0) {
      if (upd.messageDate + 60u < lastTs || upd.messageDate > lastTs + kSplitSyncWindowSec) {
        return {};
      }
    }
    return lastTxt;
  }

  return {};
}

bool parsePairingChatFromUpdatesJson(std::string_view updatesJson, std::string& chatIdOut, std::string& usernameOut,
                                     std::string& errOut) {
  chatIdOut.clear();
  usernameOut.clear();

  JsonDocument filter;
  JsonObject resFilter = filter["result"].add<JsonObject>();
  resFilter["message"]["chat"]["id"] = true;
  resFilter["message"]["chat"]["username"] = true;
  resFilter["message"]["from"]["username"] = true;
  resFilter["message"]["from"]["first_name"] = true;
  filter["ok"] = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, updatesJson.data(), updatesJson.size(), DeserializationOption::Filter(filter));
  if (err != DeserializationError::Ok || !(doc["ok"] | false)) {
    errOut = "Invalid response from Telegram getUpdates";
    return false;
  }

  JsonArrayConst results = doc["result"].as<JsonArrayConst>();
  if (results.isNull() || results.size() == 0) {
    errOut = "No messages found. Send /start to your bot in Telegram first.";
    return false;
  }

  for (JsonVariantConst item : results) {
    JsonVariantConst chatIdVar = item["message"]["chat"]["id"];
    if (chatIdVar.isNull()) continue;

    if (chatIdVar.is<int64_t>()) {
      chatIdOut = std::to_string(chatIdVar.as<int64_t>());
    } else if (chatIdVar.is<const char*>()) {
      chatIdOut = chatIdVar.as<const char*>();
    }
    const char* uname = item["message"]["from"]["username"] | item["message"]["from"]["first_name"] |
                        item["message"]["chat"]["username"] | "";
    usernameOut = uname;
  }

  if (chatIdOut.empty()) {
    errOut = "No chat message found in recent updates. Send /start to your bot first.";
    return false;
  }
  return true;
}

bool parseFileUpdatesJson(std::string_view updatesJson, std::string_view allowedChatId,
                          std::vector<PendingFileUpdate>& updatesOut, int64_t& highestUpdateIdOut,
                          std::string& errOut) {
  updatesOut.clear();
  highestUpdateIdOut = 0;

  JsonDocument filter;
  filter["ok"] = true;
  JsonObject resFilter = filter["result"].add<JsonObject>();
  resFilter["update_id"] = true;
  resFilter["message"]["message_id"] = true;
  resFilter["message"]["date"] = true;
  resFilter["message"]["caption"] = true;
  resFilter["message"]["chat"]["id"] = true;
  resFilter["message"]["document"]["file_id"] = true;
  resFilter["message"]["document"]["file_name"] = true;
  resFilter["message"]["document"]["file_size"] = true;
  resFilter["message"]["document"]["mime_type"] = true;
  JsonObject photoFilter = resFilter["message"]["photo"].add<JsonObject>();
  photoFilter["file_id"] = true;
  photoFilter["file_size"] = true;
  photoFilter["width"] = true;
  photoFilter["height"] = true;
  resFilter["message"]["reply_to_message"]["message_id"] = true;
  resFilter["message"]["reply_to_message"]["document"]["file_name"] = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, updatesJson.data(), updatesJson.size(), DeserializationOption::Filter(filter));
  if (err != DeserializationError::Ok || !(doc["ok"] | false)) {
    errOut = "Failed to parse Telegram getUpdates JSON";
    return false;
  }

  JsonArrayConst results = doc["result"].as<JsonArrayConst>();
  if (results.isNull()) {
    return true;
  }

  for (JsonVariantConst item : results) {
    const int64_t updateId = item["update_id"] | static_cast<int64_t>(0);
    if (updateId > highestUpdateIdOut) {
      highestUpdateIdOut = updateId;
    }

    JsonVariantConst msgNode = item["message"];
    if (msgNode.isNull()) continue;

    JsonVariantConst chatIdVar = msgNode["chat"]["id"];
    std::string chatIdStr;
    if (chatIdVar.is<int64_t>()) {
      chatIdStr = std::to_string(chatIdVar.as<int64_t>());
    } else if (chatIdVar.is<const char*>()) {
      chatIdStr = chatIdVar.as<const char*>();
    }
    if (chatIdStr.empty() || chatIdStr != allowedChatId) {
      continue;
    }

    const int64_t messageId = msgNode["message_id"] | static_cast<int64_t>(0);
    const uint32_t messageDate = msgNode["date"] | static_cast<uint32_t>(0);
    const char* captionRaw = msgNode["caption"] | "";
    const int64_t replyMsgId = msgNode["reply_to_message"]["message_id"] | static_cast<int64_t>(0);
    const char* replyDocName = msgNode["reply_to_message"]["document"]["file_name"] | "";

    JsonVariantConst docNode = msgNode["document"];
    if (!docNode.isNull()) {
      const char* fileId = docNode["file_id"] | "";
      const char* rawName = docNode["file_name"] | "";
      const char* mimeType = docNode["mime_type"] | "";
      const size_t fileSize = docNode["file_size"] | static_cast<size_t>(0);

      if (fileId[0] == '\0' || fileSize > kMaxTelegramFileBytes) continue;

      bool isCoverImg = false;
      std::string effectiveRawName = rawName;
      if (!effectiveRawName.empty() && isSupportedBookExtension(effectiveRawName)) {
        isCoverImg = false;
      } else if (!effectiveRawName.empty() && isSupportedImageExtension(effectiveRawName)) {
        isCoverImg = true;
      } else if (const char* imgExt = imageExtFromMime(mimeType)) {
        isCoverImg = true;
        if (effectiveRawName.empty()) {
          effectiveRawName = "tg_cover_" + std::to_string(messageId > 0 ? messageId : updateId) + std::string(imgExt);
        }
      } else {
        continue;
      }

      const std::string sanitized = StringUtils::sanitizeFilenamePreservingExtension(effectiveRawName);
      if (sanitized.empty() || sanitized.find("..") != std::string::npos || sanitized.find('/') != std::string::npos) {
        continue;
      }

      PendingFileUpdate upd;
      upd.updateId = updateId;
      upd.messageId = messageId;
      upd.messageDate = messageDate;
      upd.isCoverImage = isCoverImg;
      upd.fileId = fileId;
      upd.fileName = sanitized;
      upd.fileSize = fileSize;
      upd.caption = captionRaw;
      upd.replyToMessageId = replyMsgId;
      upd.replyToFileName = replyDocName;
      updatesOut.push_back(std::move(upd));
      continue;
    }

    JsonArrayConst photos = msgNode["photo"].as<JsonArrayConst>();
    if (!photos.isNull() && photos.size() > 0) {
      // Prefer largest valid variant <= 600px (~320x480 / ~20 KB); if all variants exceed 600px,
      // pick the smallest valid variant by max dimension so ESP32-C3 never downloads multi-MB originals unnecessarily.
      JsonVariantConst chosen;
      int bestSmallMaxDim = -1;
      JsonVariantConst smallestOversize;
      int smallestOversizeDim = INT_MAX;

      for (JsonVariantConst ph : photos) {
        const char* fid = ph["file_id"] | "";
        const size_t fsz = ph["file_size"] | static_cast<size_t>(0);
        if (fid[0] == '\0' || fsz > kMaxTelegramFileBytes) continue;
        const int w = ph["width"] | 0;
        const int h = ph["height"] | 0;
        const int maxDim = std::max(w, h);
        if (maxDim > 0 && maxDim <= 600) {
          if (maxDim >= bestSmallMaxDim) {
            chosen = ph;
            bestSmallMaxDim = maxDim;
          }
        } else {
          const int candidateDim = maxDim > 0 ? maxDim : (INT_MAX - 1);
          if (candidateDim < smallestOversizeDim) {
            smallestOversize = ph;
            smallestOversizeDim = candidateDim;
          }
        }
      }
      if (chosen.isNull()) {
        chosen = smallestOversize;
      }

      const char* chosenId = chosen["file_id"] | "";
      const size_t chosenSize = chosen["file_size"] | static_cast<size_t>(0);
      if (chosenId[0] != '\0' && chosenSize <= kMaxTelegramFileBytes) {
        PendingFileUpdate upd;
        upd.updateId = updateId;
        upd.messageId = messageId;
        upd.messageDate = messageDate;
        upd.isCoverImage = true;
        upd.fileId = chosenId;
        upd.fileName = "tg_cover_" + std::to_string(messageId > 0 ? messageId : updateId) + ".jpg";
        upd.fileSize = chosenSize;
        upd.caption = captionRaw;
        upd.replyToMessageId = replyMsgId;
        upd.replyToFileName = replyDocName;
        updatesOut.push_back(std::move(upd));
      }
    }
  }
  return true;
}

bool parseGetFilePathJson(std::string_view getFileJson, std::string& filePathOut, std::string& errOut) {
  filePathOut.clear();
  JsonDocument doc;
  if (deserializeJson(doc, getFileJson.data(), getFileJson.size()) != DeserializationError::Ok ||
      !(doc["ok"] | false)) {
    errOut = "Invalid response from Telegram getFile";
    return false;
  }
  const char* fp = doc["result"]["file_path"] | "";
  if (fp[0] == '\0') {
    errOut = "Missing result.file_path in Telegram getFile response";
    return false;
  }
  filePathOut = fp;
  return true;
}

bool pairLatestChat(std::string_view botToken, std::string& chatIdOut, std::string& usernameOut, std::string& errOut) {
  if (botToken.empty()) {
    errOut = "Bot token is empty";
    return false;
  }

  const std::string url =
      std::string(kTelegramApiBase) + "/bot" + std::string(botToken) + "/getUpdates?limit=10&timeout=0";
  String resp;
  const int status = pluginhttp::request(nullptr, url, "GET", "", {{"Accept", "application/json"}}, resp, 32 * 1024);
  if (status < 200 || status >= 300 || resp.length() == 0) {
    errOut = "Telegram getUpdates failed (HTTP " + std::to_string(status) + ")";
    return false;
  }

  const bool parsed =
      parsePairingChatFromUpdatesJson(std::string_view(resp.c_str(), resp.length()), chatIdOut, usernameOut, errOut);
  resp = String();
  if (!parsed) {
    return false;
  }

  TELEGRAM_STORE.setBotToken(std::string(botToken));
  TELEGRAM_STORE.setAllowedChatId(chatIdOut);
  TELEGRAM_STORE.saveToFile();
  return true;
}

bool syncPendingFiles(SyncSummary& summaryOut, std::string& errOut, ProgressCallback progressCb,
                      const std::function<bool()>& shouldStop) {
  summaryOut = {};
  if (!TELEGRAM_STORE.hasCredentials()) {
    errOut = "Telegram bot token or chat ID not configured";
    return false;
  }

  const std::string botToken = TELEGRAM_STORE.getBotToken();
  const std::string allowedChatId = TELEGRAM_STORE.getAllowedChatId();
  const std::string destFolder = TELEGRAM_STORE.getDestFolder();
  int64_t nextOffset = TELEGRAM_STORE.getLastUpdateId() + 1;

  // Reuse a single TLS session across consecutive Telegram Bot API JSON requests; release it
  // before streaming file downloads so two 17 KB TLS record buffers never coexist in DRAM.
  std::unique_ptr<freeink::SecureHttpClient> apiSession;
  auto ensureApiSession = [&]() -> freeink::SecureHttpClient* {
    if (!apiSession) {
      apiSession = makeUniqueNoThrow<freeink::SecureHttpClient>();
    }
    return apiSession.get();
  };

  for (int batch = 0; batch < 10; ++batch) {
    // Batches are the unit of progress: the update offset is saved after each one.
    if (shouldStop && shouldStop()) break;
    const std::string updatesUrl = std::string(kTelegramApiBase) + "/bot" + botToken +
                                   "/getUpdates?offset=" + std::to_string(nextOffset) +
                                   "&limit=4&timeout=0&allowed_updates=%5B%22message%22%5D";
    String updatesResp;
    const int status = pluginhttp::request(ensureApiSession(), updatesUrl, "GET", "", {{"Accept", "application/json"}},
                                           updatesResp, 32 * 1024);
    if (status < 200 || status >= 300 || updatesResp.length() == 0) {
      errOut = "Telegram getUpdates failed (HTTP " + std::to_string(status) + ")";
      return false;
    }

    std::vector<PendingFileUpdate> updates;
    int64_t highestUpdateId = 0;
    const bool parsed = parseFileUpdatesJson(std::string_view(updatesResp.c_str(), updatesResp.length()), allowedChatId,
                                             updates, highestUpdateId, errOut);
    updatesResp = String();  // Free response buffer immediately after JSON parse
    if (!parsed) {
      return false;
    }

    if (highestUpdateId == 0 || highestUpdateId < nextOffset) {
      break;  // No more pending updates in queue
    }

    for (const auto& upd : updates) {
      summaryOut.updatesProcessed++;
      const std::string getFileUrl = std::string(kTelegramApiBase) + "/bot" + botToken +
                                     "/getFile?file_id=" + pluginhttp::urlEncodeQuery(upd.fileId);
      String getFileResp;
      const int gfStatus = pluginhttp::request(ensureApiSession(), getFileUrl, "GET", "",
                                               {{"Accept", "application/json"}}, getFileResp, 8 * 1024);
      if (gfStatus < 200 || gfStatus >= 300 || getFileResp.length() == 0) {
        continue;
      }

      std::string remoteFilePath;
      const bool gotPath =
          parseGetFilePathJson(std::string_view(getFileResp.c_str(), getFileResp.length()), remoteFilePath, errOut);
      getFileResp = String();
      if (!gotPath) {
        continue;
      }

      const std::string fileDownloadUrl =
          std::string(kTelegramApiBase) + "/file/bot" + botToken + "/" + pluginhttp::escapeUrl(remoteFilePath);

      // Release Bot API TLS session before opening HttpDownloader's TLS stream.
      apiSession.reset();

      if (upd.isCoverImage) {
        const std::string targetTxtPath = resolveCoverTargetTxtPath(upd, TELEGRAM_STORE);
        if (!targetTxtPath.empty()) {
          if (!Storage.exists("/.crosspoint")) Storage.mkdir("/.crosspoint");
          if (!Storage.exists(txtmeta::kHiddenCoversDir)) Storage.mkdir(txtmeta::kHiddenCoversDir);

          const char* tmpExt = ".jpg";
          if (hasSuffixIgnoreCase(upd.fileName, ".png")) {
            tmpExt = ".png";
          } else if (hasSuffixIgnoreCase(upd.fileName, ".bmp")) {
            tmpExt = ".bmp";
          }
          const std::string tempImgPath = std::string(txtmeta::kHiddenCoversDir) + "/.tg_cover_dl" + tmpExt;
          const auto dlErr = HttpDownloader::downloadToFile(fileDownloadUrl, tempImgPath, [&](size_t dl, size_t tot) {
            if (progressCb) progressCb(upd.fileName, dl, tot);
          });
          if (dlErr != HttpDownloader::OK) {
            LOG_ERR("TGS", "Failed to download Telegram cover image %s", upd.fileName.c_str());
            Storage.remove(tempImgPath.c_str());
            continue;
          }

          std::string capTitle;
          std::string capAuthor;
          const std::string* optTitle = nullptr;
          const std::string* optAuthor = nullptr;
          if (!upd.caption.empty() && txtmeta::parseTitleAuthorFromStem(upd.caption, capTitle, capAuthor)) {
            optTitle = &capTitle;
            optAuthor = &capAuthor;
          }

          const bool bound =
              BookCoverStore::bindCoverAndMetaFromImageFile(targetTxtPath, tempImgPath, optTitle, optAuthor);
          Storage.remove(tempImgPath.c_str());
          if (bound) {
            summaryOut.coversBound++;
            sendConfirmationReply(ensureApiSession(), botToken, allowedChatId, "Bound cover to: " + targetTxtPath);
          } else {
            LOG_ERR("TGS", "Failed to bind Telegram cover image to %s", targetTxtPath.c_str());
          }
        } else {
          // Unmatched cover photo: save cleanly into /covers/ (never /Books/) for on-device picking
          const std::string coversDir = "/covers";
          if (!Storage.exists(coversDir.c_str())) {
            Storage.mkdir(coversDir.c_str());
          }
          const std::string savedCoverPath = coversDir + "/" + upd.fileName;
          const auto dlErr =
              HttpDownloader::downloadToFile(fileDownloadUrl, savedCoverPath, [&](size_t dl, size_t tot) {
                if (progressCb) progressCb(upd.fileName, dl, tot);
              });
          if (dlErr != HttpDownloader::OK) {
            LOG_ERR("TGS", "Failed to download Telegram cover image %s", upd.fileName.c_str());
            continue;
          }
          summaryOut.filesDownloaded++;
          summaryOut.savedPaths.push_back(savedCoverPath);
          sendConfirmationReply(ensureApiSession(), botToken, allowedChatId, "Saved cover image to: " + savedCoverPath);
        }
        continue;
      }

      const bool isFont = hasSuffixIgnoreCase(upd.fileName, ".cpfont");
      std::string destPath;
      if (isFont) {
        destPath = resolveCpfontDestPath(upd.fileName);
        if (destPath.empty()) {
          LOG_ERR("TGS", "Rejected invalid .cpfont filename %s", upd.fileName.c_str());
          continue;
        }
        const size_t lastSlash = destPath.rfind('/');
        const size_t firstSlash = destPath.find('/', 1);
        if (firstSlash != std::string::npos) {
          const std::string rootDir = destPath.substr(0, firstSlash);
          if (!Storage.exists(rootDir.c_str())) Storage.mkdir(rootDir.c_str());
        }
        if (lastSlash != std::string::npos && lastSlash > 0) {
          const std::string familyDir = destPath.substr(0, lastSlash);
          if (!Storage.exists(familyDir.c_str())) Storage.mkdir(familyDir.c_str());
        }
      } else {
        if (!Storage.exists(destFolder.c_str())) {
          Storage.mkdir(destFolder.c_str());
        }
        destPath = destFolder + "/" + upd.fileName;
      }

      const auto dlErr = HttpDownloader::downloadToFile(fileDownloadUrl, destPath, [&](size_t dl, size_t tot) {
        if (progressCb) progressCb(upd.fileName, dl, tot);
      });
      if (dlErr != HttpDownloader::OK) {
        LOG_ERR("TGS", "Failed to download Telegram file %s", upd.fileName.c_str());
        continue;
      }

      if (isFont) {
        sdFontSystem.markRegistryDirty();
      } else if (adept::isAcsmFile(destPath)) {
        std::string fulfilledPath;
        std::string acsmErr;
        if (adept::fulfillAcsmFile(destPath, fulfilledPath, acsmErr)) {
          destPath = fulfilledPath;
        } else {
          const std::string safeErr = redactBotToken(acsmErr, botToken);
          LOG_ERR("TGS", "ACSM fulfillment failed for %s: %s", destPath.c_str(), safeErr.c_str());
        }
      }

      if (!isFont) {
        clearBookCache(destPath);
        if (txtmeta::isTxtOrMdPath(destPath) && !upd.caption.empty()) {
          std::string capTitle;
          std::string capAuthor;
          if (txtmeta::parseTitleAuthorFromStem(upd.caption, capTitle, capAuthor)) {
            BookCoverStore::bindMetadataOnly(destPath, capTitle, capAuthor);
          } else {
            const std::string_view singleTitle = trimCaptionSpaces(upd.caption);
            if (!singleTitle.empty() && singleTitle.find('\n') == std::string_view::npos) {
              txtmeta::BoundBookMeta existingMeta;
              BookCoverStore::readBoundMeta(destPath, existingMeta);
              BookCoverStore::bindMetadataOnly(destPath, std::string(singleTitle), existingMeta.author);
            }
          }
        }

        const uint32_t msgEpoch = upd.messageDate > 0 ? upd.messageDate : 0;
        TELEGRAM_STORE.recordSyncedBookMessage(upd.messageId, destPath, msgEpoch);

        if (library::isLibraryBookFile(destPath)) {
          library::markLibraryIndexDirty();
        }
      }

      summaryOut.filesDownloaded++;
      summaryOut.savedPaths.push_back(destPath);
      sendConfirmationReply(ensureApiSession(), botToken, allowedChatId, "Saved to CrossPoint: " + destPath);
    }

    nextOffset = highestUpdateId + 1;
    TELEGRAM_STORE.setLastUpdateId(highestUpdateId);
    TELEGRAM_STORE.saveToFile();
  }

  return true;
}

}  // namespace telegram
