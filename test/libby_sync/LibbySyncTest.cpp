#include <ArduinoJson.h>
#include <TrustedTime.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "HostSyncFakes.h"
#include "network/LibbyClient.h"
#include "util/Iso8601.h"

namespace {

class LibbySyncTest : public ::testing::Test {
 protected:
  void SetUp() override { hostnet::reset(); }
};

}  // namespace

TEST_F(LibbySyncTest, ObfuscatedTokenRoundTripCrcTamperRejectionAndLegacyFallback) {
  EXPECT_FALSE(libby::hasToken());
  ASSERT_TRUE(libby::saveToken("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.secret"));

  const std::string storedJson = hostfs::readString("/.crosspoint/libby.json");
  EXPECT_NE(storedJson.find("token_obf"), std::string::npos);
  EXPECT_NE(storedJson.find("token_crc32"), std::string::npos);
  // Raw plaintext JWT must NOT appear on disk
  EXPECT_EQ(storedJson.find("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"), std::string::npos);

  std::string loaded;
  ASSERT_TRUE(libby::loadToken(loaded));
  EXPECT_EQ(loaded, "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.secret");

  // Tamper with token_crc32 -> loadToken rejects corrupted credential
  JsonDocument doc;
  ASSERT_EQ(deserializeJson(doc, storedJson), DeserializationError::Ok);
  doc["token_crc32"] = doc["token_crc32"].as<uint32_t>() ^ 0x12345678u;
  std::string tampered;
  serializeJson(doc, tampered);
  hostfs::writeString("/.crosspoint/libby.json", tampered);
  EXPECT_FALSE(libby::loadToken(loaded));

  // Legacy plugin token.json plaintext fallback remains supported
  hostfs::reset();
  hostfs::writeString("/.crosspoint/plugins/libby/token.json", R"json({"identity":"legacy-bearer-xyz"})json");
  ASSERT_TRUE(libby::loadToken(loaded));
  EXPECT_EQ(loaded, "legacy-bearer-xyz");
}

TEST_F(LibbySyncTest, StreamsAndFiltersSyncLoansFromMemoryAndFile) {
  const char* syncJson = R"json({
    "result": "ok",
    "cards": [{"cardId": "card-1", "advantageKey": "spl", "hugeIgnoredBlob": "xxxxxxxxxxxxxxxxxxxxxxxx"}],
    "loans": [
      {
        "id": "loan-open-1",
        "cardId": "card-1",
        "title": "Pride and Prejudice",
        "firstCreatorName": "Jane Austen",
        "expireDate": "2026-11-30T00:00:00Z",
        "formats": [{"id": "ebook-overdrive"}, {"id": "ebook-epub-open"}]
      },
      {
        "id": "loan-adobe-2",
        "cardId": "card-1",
        "title": "三体 (The Three-Body Problem)",
        "firstCreatorName": "Liu Cixin",
        "expireDate": "2026-12-05T00:00:00Z",
        "formats": [{"id": "ebook-overdrive"}, {"id": "ebook-epub-adobe"}]
      },
      {
        "id": "loan-audio-only",
        "cardId": "card-1",
        "title": "Audiobook Only",
        "firstCreatorName": "Narrator",
        "formats": [{"id": "audiobook-mp3"}]
      }
    ]
  })json";

  std::vector<libby::Loan> memLoans;
  std::string err;
  ASSERT_TRUE(libby::parseSyncLoansJson(syncJson, memLoans, err));
  ASSERT_EQ(memLoans.size(), 3u);
  EXPECT_TRUE(memLoans[0].hasOpenEpub);
  EXPECT_FALSE(memLoans[0].hasAdobeEpub);
  EXPECT_TRUE(memLoans[1].hasAdobeEpub);
  EXPECT_FALSE(memLoans[2].isDownloadable());

  hostfs::writeString("/.crosspoint/libby_sync.tmp", syncJson);
  std::vector<libby::Loan> fileLoans;
  ASSERT_TRUE(libby::parseSyncLoansFile("/.crosspoint/libby_sync.tmp", fileLoans, err));
  ASSERT_EQ(fileLoans.size(), 3u);
  EXPECT_EQ(fileLoans[1].title, "三体 (The Three-Body Problem)");
}

TEST_F(LibbySyncTest, UrlEncodesFulfillEndpointAndEscapesSidecarJson) {
  libby::Loan loan;
  loan.cardId = "card/with space";
  loan.id = "loan#42&x=1";
  loan.title = "Book \"Quoted\" \\ Edition";
  loan.hasAdobeEpub = true;

  const std::string url = libby::buildFulfillEndpointUrl(loan);
  EXPECT_EQ(url,
            "https://sentry-read.svc.overdrive.com/card/card%2Fwith%20space/loan/loan%2342%26x%3D1/fulfill/"
            "ebook-epub-adobe");

  const std::string sidecar = libby::serializeLoanSidecarJson(loan);
  JsonDocument parsed;
  ASSERT_EQ(deserializeJson(parsed, sidecar), DeserializationError::Ok);
  EXPECT_STREQ(parsed["libby_loan_id"] | "", "loan#42&x=1");
  EXPECT_STREQ(parsed["libby_card_id"] | "", "card/with space");
}

TEST_F(LibbySyncTest, ParsesIso8601UtcAndBuildsLoanDestPath) {
  const int64_t now = iso8601::parseUtc("2026-11-16T12:00:00Z");
  const int64_t due = iso8601::parseUtc("2026-11-30T12:00:00Z");
  EXPECT_EQ(due - now, 14 * 86400);

  libby::Loan sampleLoan;
  sampleLoan.id = "loan-open-1";
  sampleLoan.cardId = "card-1";
  sampleLoan.title = "Pride and Prejudice";
  sampleLoan.expireDate = "2026-11-30T12:00:00Z";
  sampleLoan.hasOpenEpub = true;

  EXPECT_EQ(libby::loanDestEpubPath(sampleLoan), "/Libby/Pride and Prejudice.epub");
}
