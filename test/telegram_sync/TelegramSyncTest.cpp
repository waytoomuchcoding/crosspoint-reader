#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "HostSyncFakes.h"
#include "SdCardFontSystem.h"
#include "TelegramCredentialStore.h"
#include "network/TelegramBookSync.h"
#include "util/BookCoverStore.h"

namespace {

struct BoundCoverEvent {
  std::string bookPath;
  std::string imgPath;
  std::string title;
  std::string author;
};

std::vector<BoundCoverEvent> g_boundCovers;
std::vector<BoundCoverEvent> g_boundMetaOnly;

class TelegramSyncTest : public ::testing::Test {
 protected:
  void SetUp() override {
    hostnet::reset();
    g_boundCovers.clear();
    g_boundMetaOnly.clear();
    sdFontSystem.dirty = false;
    TELEGRAM_STORE.clearCredentials();
  }
};

}  // namespace

bool BookCoverStore::bindCoverAndMetaFromImageFile(const std::string& bookPath, const std::string& sourceImagePath,
                                                   const std::string* optionalTitle,
                                                   const std::string* optionalAuthor) {
  g_boundCovers.push_back(
      {bookPath, sourceImagePath, optionalTitle ? *optionalTitle : "", optionalAuthor ? *optionalAuthor : ""});
  return true;
}

bool BookCoverStore::bindMetadataOnly(const std::string& bookPath, const std::string& title,
                                      const std::string& author) {
  g_boundMetaOnly.push_back({bookPath, "", title, author});
  return true;
}

bool BookCoverStore::readBoundMeta(std::string_view, txtmeta::BoundBookMeta& out) {
  out = {};
  return false;
}

TEST_F(TelegramSyncTest, CredentialStoreObfuscatesBotTokenAndSanitizesDestFolder) {
  TELEGRAM_STORE.setBotToken("123456:ABCDEF-SECRET-BOT-TOKEN");
  TELEGRAM_STORE.setAllowedChatId("987654321");

  // Traversal, root, hidden dot-directories, and protected paths fall back safely to /Books
  TELEGRAM_STORE.setDestFolder("/Books/../.crosspoint");
  EXPECT_EQ(TELEGRAM_STORE.getDestFolder(), "/Books");
  TELEGRAM_STORE.setDestFolder("/.crosspoint");
  EXPECT_EQ(TELEGRAM_STORE.getDestFolder(), "/Books");
  TELEGRAM_STORE.setDestFolder("/.fonts");
  EXPECT_EQ(TELEGRAM_STORE.getDestFolder(), "/Books");
  TELEGRAM_STORE.setDestFolder("/");
  EXPECT_EQ(TELEGRAM_STORE.getDestFolder(), "/Books");

  TELEGRAM_STORE.setDestFolder("Reading/Novels/");
  EXPECT_EQ(TELEGRAM_STORE.getDestFolder(), "/Reading/Novels");

  JsonDocument doc;
  TELEGRAM_STORE.toJson(doc);
  std::string serialized;
  serializeJson(doc, serialized);
  EXPECT_EQ(serialized.find("ABCDEF-SECRET-BOT-TOKEN"), std::string::npos);

  // Valid round-trip
  TELEGRAM_STORE.clearCredentials();
  TELEGRAM_STORE.fromJson(doc.as<JsonVariantConst>());
  EXPECT_EQ(TELEGRAM_STORE.getBotToken(), "123456:ABCDEF-SECRET-BOT-TOKEN");
  EXPECT_EQ(TELEGRAM_STORE.getDestFolder(), "/Reading/Novels");

  // CRC32 tamper rejection
  doc["botToken_crc32"] = doc["botToken_crc32"].as<uint32_t>() ^ 0xFF00FF00u;
  TELEGRAM_STORE.clearCredentials();
  TELEGRAM_STORE.fromJson(doc.as<JsonVariantConst>());
  EXPECT_TRUE(TELEGRAM_STORE.getBotToken().empty());
}

TEST_F(TelegramSyncTest, RedactsBotTokenAndRoutesCpfontIntoFamilySubfolder) {
  EXPECT_EQ(telegram::redactBotToken("GET https://api.telegram.org/bot999:SECRET/getUpdates failed (999:SECRET)",
                                     "999:SECRET"),
            "GET https://api.telegram.org/bot<redacted>/getUpdates failed (<redacted>)");

  EXPECT_EQ(telegram::extractCpfontFamilyName("NotoSansSC_16.cpfont"), "NotoSansSC");
  EXPECT_EQ(telegram::extractCpfontFamilyName("LXGW_WenKai_18_bold.cpfont"), "LXGW_WenKai");
  EXPECT_EQ(telegram::resolveCpfontDestPath("NotoSansSC_16.cpfont"), "/fonts/NotoSansSC/NotoSansSC_16.cpfont");

  Storage.mkdir("/.fonts");
  EXPECT_EQ(telegram::resolveCpfontDestPath("LXGW_WenKai_18_bold.cpfont"),
            "/.fonts/LXGW_WenKai/LXGW_WenKai_18_bold.cpfont");
}

TEST_F(TelegramSyncTest, SelectsSmallestPhotoAbove600PxWhenAllVariantsExceed600Px) {
  const char* updatesJson = R"json({
    "ok": true,
    "result": [
      {
        "update_id": 10,
        "message": {
          "message_id": 77,
          "date": 1700000100,
          "chat": {"id": 987654321},
          "photo": [
            {"file_id": "PH_1280", "width": 853, "height": 1280, "file_size": 150000},
            {"file_id": "PH_800", "width": 533, "height": 800, "file_size": 64000}
          ]
        }
      }
    ]
  })json";

  std::vector<telegram::PendingFileUpdate> updates;
  int64_t maxId = 0;
  std::string err;
  ASSERT_TRUE(telegram::parseFileUpdatesJson(updatesJson, "987654321", updates, maxId, err));
  ASSERT_EQ(updates.size(), 1u);
  EXPECT_EQ(updates[0].fileId, "PH_800");
  EXPECT_EQ(updates[0].messageDate, 1700000100u);
}

TEST_F(TelegramSyncTest, PairsCoversAcrossSameBatchSplitSyncReplyAndExpiresStaleSequentialWindow) {
  TELEGRAM_STORE.setBotToken("TESTTOKEN");
  TELEGRAM_STORE.setAllowedChatId("987654321");
  TELEGRAM_STORE.setDestFolder("/Books");
  TELEGRAM_STORE.setLastUpdateId(2000);
  hostfs::writeString("/Books/legacy_old.txt", "OLD_BOOK");

  // 1. Same-batch .txt upload (with CJK book-title caption) + immediate photo
  const char* sync1Batch = R"json({
    "ok": true,
    "result": [
      {
        "update_id": 2001,
        "message": {
          "message_id": 501,
          "date": 1700001000,
          "caption": "《十日终焉》杀虫队队员",
          "chat": {"id": 987654321},
          "document": {"file_id": "DOC_BOOK_A", "file_name": "book_a.txt", "file_size": 12000}
        }
      },
      {
        "update_id": 2002,
        "message": {
          "message_id": 502,
          "date": 1700001020,
          "chat": {"id": 987654321},
          "photo": [
            {"file_id": "PH_90", "width": 60, "height": 90, "file_size": 1800},
            {"file_id": "PH_480", "width": 320, "height": 480, "file_size": 21500},
            {"file_id": "PH_1280", "width": 853, "height": 1280, "file_size": 148000}
          ]
        }
      }
    ]
  })json";
  hostnet::httpQueue().push_back({"offset=2001", sync1Batch});
  hostnet::httpQueue().push_back({"offset=2003", R"json({"ok":true,"result":[]})json"});

  telegram::SyncSummary sum1;
  std::string err;
  ASSERT_TRUE(telegram::syncPendingFiles(sum1, err));
  EXPECT_EQ(sum1.filesDownloaded, 1u);
  EXPECT_EQ(sum1.coversBound, 1u);
  ASSERT_EQ(g_boundMetaOnly.size(), 1u);
  EXPECT_EQ(g_boundMetaOnly[0].title, "十日终焉");
  EXPECT_EQ(g_boundMetaOnly[0].author, "杀虫队队员");
  ASSERT_EQ(g_boundCovers.size(), 1u);
  EXPECT_EQ(g_boundCovers[0].bookPath, "/Books/book_a.txt");

  // 2. Split-sync: book_b.txt arrives at t=1700002000; photo arrives 2 hours later (t=1700009200 > +3600s) ->
  //    sequential pairing window expired so un-replied photo goes to /covers/ instead of overwriting book_b.txt!
  //    Meanwhile explicit reply_to_message (id=501) still binds to book_a.txt regardless of age!
  g_boundCovers.clear();
  const char* sync2Batch = R"json({
    "ok": true,
    "result": [
      {
        "update_id": 2003,
        "message": {
          "message_id": 503,
          "date": 1700002000,
          "chat": {"id": 987654321},
          "document": {"file_id": "DOC_BOOK_B", "file_name": "book_b.txt", "file_size": 8192}
        }
      },
      {
        "update_id": 2004,
        "message": {
          "message_id": 504,
          "date": 1700009200,
          "chat": {"id": 987654321},
          "photo": [{"file_id": "PH_STALE", "width": 320, "height": 480, "file_size": 19000}]
        }
      },
      {
        "update_id": 2005,
        "message": {
          "message_id": 505,
          "date": 1700009300,
          "chat": {"id": 987654321},
          "reply_to_message": {"message_id": 501},
          "photo": [{"file_id": "PH_REPLY_A", "width": 320, "height": 480, "file_size": 20000}]
        }
      }
    ]
  })json";
  hostnet::httpQueue().clear();
  hostnet::httpQueue().push_back({"offset=2003", sync2Batch});
  hostnet::httpQueue().push_back({"offset=2006", R"json({"ok":true,"result":[]})json"});

  telegram::SyncSummary sum2;
  ASSERT_TRUE(telegram::syncPendingFiles(sum2, err));
  EXPECT_EQ(sum2.coversBound, 1u);
  ASSERT_EQ(g_boundCovers.size(), 1u);
  EXPECT_EQ(g_boundCovers[0].bookPath, "/Books/book_a.txt");
  EXPECT_TRUE(Storage.exists("/covers/tg_cover_504.jpg"));
}
