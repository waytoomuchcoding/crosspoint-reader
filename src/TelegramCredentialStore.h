#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

class TelegramCredentialStore : public PersistableStore<TelegramCredentialStore> {
 public:
  static constexpr size_t kMaxRecentMessages = 8;

  struct RecentMessageBinding {
    int64_t messageId = 0;
    std::string savedPath;
  };

 private:
  std::string botToken_;
  std::string allowedChatId_;
  std::string destFolder_ = "/Books";
  int64_t lastUpdateId_ = 0;
  bool autoSyncOnSleep_ = false;
  std::string lastSyncedTxtPath_;
  uint32_t lastSyncedTxtTimestamp_ = 0;
  std::array<RecentMessageBinding, kMaxRecentMessages> recentMessages_{};
  size_t recentMessageCount_ = 0;

  TelegramCredentialStore() = default;
  ~TelegramCredentialStore() = default;

  friend class PersistableStore<TelegramCredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/telegram.json"; }

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  void setBotToken(const std::string& token) { botToken_ = token; }
  const std::string& getBotToken() const { return botToken_; }

  void setAllowedChatId(const std::string& chatId) { allowedChatId_ = chatId; }
  const std::string& getAllowedChatId() const { return allowedChatId_; }

  void setDestFolder(const std::string& folder);
  const std::string& getDestFolder() const { return destFolder_; }

  void setLastUpdateId(int64_t updateId) { lastUpdateId_ = updateId; }
  int64_t getLastUpdateId() const { return lastUpdateId_; }

  void setAutoSyncOnSleep(bool enabled) { autoSyncOnSleep_ = enabled; }
  bool getAutoSyncOnSleep() const { return autoSyncOnSleep_; }

  void recordSyncedBookMessage(int64_t messageId, const std::string& savedPath, uint32_t timestamp = 0);
  std::string findBookPathByMessageId(int64_t messageId) const;

  void setLastSyncedTxtPath(const std::string& path, uint32_t timestamp = 0) {
    lastSyncedTxtPath_ = path;
    lastSyncedTxtTimestamp_ = timestamp;
  }
  const std::string& getLastSyncedTxtPath() const { return lastSyncedTxtPath_; }
  uint32_t getLastSyncedTxtTimestamp() const { return lastSyncedTxtTimestamp_; }
  size_t getRecentMessageCount() const { return recentMessageCount_; }

  bool hasCredentials() const { return !botToken_.empty() && !allowedChatId_.empty(); }
  void clearCredentials();
};

#define TELEGRAM_STORE TelegramCredentialStore::getInstance()
