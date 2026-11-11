#include "TelegramCredentialStore.h"

#include <CredentialIntegrity.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <TxtCoverMeta.h>

#include "network/ProtectedPaths.h"

void TelegramCredentialStore::toJson(JsonDocument& doc) const {
  doc["botToken_obf"] = obfuscation::obfuscateToBase64(botToken_);
  doc["botToken_crc32"] = credential_integrity::crc32(botToken_);
  doc["allowedChatId"] = allowedChatId_;
  doc["destFolder"] = destFolder_;
  doc["lastUpdateId"] = lastUpdateId_;
  doc["autoSyncOnSleep"] = autoSyncOnSleep_;
  if (!lastSyncedTxtPath_.empty()) {
    doc["lastSyncedTxtPath"] = lastSyncedTxtPath_;
    doc["lastSyncedTxtTimestamp"] = lastSyncedTxtTimestamp_;
  }
  if (recentMessageCount_ > 0) {
    JsonArray arr = doc["recentMessages"].to<JsonArray>();
    for (size_t i = 0; i < recentMessageCount_; ++i) {
      if (recentMessages_[i].messageId <= 0 || recentMessages_[i].savedPath.empty()) continue;
      JsonObject item = arr.add<JsonObject>();
      item["id"] = recentMessages_[i].messageId;
      item["path"] = recentMessages_[i].savedPath;
    }
  }
}

bool TelegramCredentialStore::fromJson(JsonVariantConst doc) {
  bool needsResave = false;
  const char* obf = doc["botToken_obf"] | "";
  const char* plain = doc["botToken"] | "";
  if (obf[0] != '\0') {
    bool ok = false;
    std::string decoded = obfuscation::deobfuscateFromBase64(obf, &ok);
    if (ok && doc["botToken_crc32"].is<uint32_t>()) {
      const uint32_t expectedCrc = doc["botToken_crc32"].as<uint32_t>();
      if (!credential_integrity::validate(decoded, decoded.size(), expectedCrc)) {
        ok = false;
      }
    }
    botToken_ = ok ? std::move(decoded) : std::string();
  } else if (plain[0] != '\0') {
    botToken_ = plain;
    needsResave = true;
  } else {
    botToken_.clear();
  }

  if (doc["allowedChatId"].is<int64_t>()) {
    allowedChatId_ = std::to_string(doc["allowedChatId"].as<int64_t>());
  } else {
    allowedChatId_ = doc["allowedChatId"] | "";
  }

  setDestFolder(doc["destFolder"] | "/Books");
  lastUpdateId_ = doc["lastUpdateId"] | static_cast<int64_t>(0);
  autoSyncOnSleep_ = doc["autoSyncOnSleep"] | false;
  lastSyncedTxtPath_ = doc["lastSyncedTxtPath"] | "";
  lastSyncedTxtTimestamp_ = doc["lastSyncedTxtTimestamp"] | static_cast<uint32_t>(0);

  recentMessageCount_ = 0;
  JsonArrayConst arr = doc["recentMessages"].as<JsonArrayConst>();
  if (!arr.isNull()) {
    for (JsonVariantConst entry : arr) {
      if (recentMessageCount_ >= kMaxRecentMessages) break;
      const int64_t msgId = entry["id"] | static_cast<int64_t>(0);
      const char* path = entry["path"] | "";
      if (msgId > 0 && path[0] != '\0') {
        recentMessages_[recentMessageCount_++] = RecentMessageBinding{msgId, path};
      }
    }
  }

  if (needsResave) {
    requestResave();
  }
  return true;
}

void TelegramCredentialStore::recordSyncedBookMessage(const int64_t messageId, const std::string& savedPath,
                                                      const uint32_t timestamp) {
  if (savedPath.empty()) return;

  if (txtmeta::isTxtOrMdPath(savedPath)) {
    lastSyncedTxtPath_ = savedPath;
    lastSyncedTxtTimestamp_ = timestamp;
  } else {
    lastSyncedTxtPath_.clear();
    lastSyncedTxtTimestamp_ = 0;
  }

  if (messageId <= 0) return;

  size_t existingIdx = recentMessageCount_;
  for (size_t i = 0; i < recentMessageCount_; ++i) {
    if (recentMessages_[i].messageId == messageId) {
      existingIdx = i;
      break;
    }
  }

  if (existingIdx < recentMessageCount_) {
    for (size_t i = existingIdx; i > 0; --i) {
      recentMessages_[i] = std::move(recentMessages_[i - 1]);
    }
  } else {
    const size_t newCount = std::min(recentMessageCount_ + 1, kMaxRecentMessages);
    for (size_t i = newCount - 1; i > 0; --i) {
      recentMessages_[i] = std::move(recentMessages_[i - 1]);
    }
    recentMessageCount_ = newCount;
  }
  recentMessages_[0] = RecentMessageBinding{messageId, savedPath};
}

std::string TelegramCredentialStore::findBookPathByMessageId(const int64_t messageId) const {
  if (messageId <= 0) return {};
  for (size_t i = 0; i < recentMessageCount_; ++i) {
    if (recentMessages_[i].messageId == messageId) {
      return recentMessages_[i].savedPath;
    }
  }
  return {};
}

void TelegramCredentialStore::setDestFolder(const std::string& folder) {
  if (folder.empty() || folder.find("..") != std::string::npos) {
    destFolder_ = "/Books";
    return;
  }
  const std::string raw = (folder[0] == '/') ? folder : ("/" + folder);
  std::string norm = FsHelpers::normalisePath(raw);
  while (norm.size() > 1 && norm.back() == '/') {
    norm.pop_back();
  }
  // Reject root, hidden/system folders ("/.crosspoint", "/.fonts"), or protected paths.
  if (norm.size() <= 1 || norm[0] != '/' || norm.find("/.") != std::string::npos ||
      norm.find('\\') != std::string::npos || protectedpaths::isSensitivePath(norm.c_str())) {
    destFolder_ = "/Books";
    return;
  }
  destFolder_ = std::move(norm);
}

void TelegramCredentialStore::clearCredentials() {
  botToken_.clear();
  allowedChatId_.clear();
  lastUpdateId_ = 0;
  lastSyncedTxtPath_.clear();
  lastSyncedTxtTimestamp_ = 0;
  recentMessageCount_ = 0;
  saveToFile();
}
