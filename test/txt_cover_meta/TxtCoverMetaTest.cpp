#include <Epub/BookMetadataCache.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Memory.h>
#include <Txt.h>
#include <TxtCoverMeta.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "util/BookCoverStore.h"

namespace {

bool g_indexMarkedDirty = false;
std::string g_lastUpdatedRecentPath;
std::string g_lastUpdatedRecentTitle;

std::vector<uint8_t> buildValid1BitBmp(const int width, const int height) {
  const size_t rowBytes = (static_cast<size_t>(width) + 31u) / 32u * 4u;
  const size_t pixelOffset = 62u;
  const size_t totalSize = pixelOffset + rowBytes * static_cast<size_t>(height);
  std::vector<uint8_t> bmp(totalSize, 0xFF);
  auto writeLe16 = [&](size_t off, uint16_t v) {
    bmp[off] = static_cast<uint8_t>(v & 0xFFu);
    bmp[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  };
  auto writeLe32 = [&](size_t off, uint32_t v) {
    bmp[off] = static_cast<uint8_t>(v & 0xFFu);
    bmp[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    bmp[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    bmp[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
  };
  bmp[0] = 'B';
  bmp[1] = 'M';
  writeLe32(2, static_cast<uint32_t>(totalSize));
  writeLe32(6, 0);
  writeLe32(10, static_cast<uint32_t>(pixelOffset));
  writeLe32(14, 40u);
  writeLe32(18, static_cast<uint32_t>(width));
  writeLe32(22, static_cast<uint32_t>(height));
  writeLe16(26, 1u);
  writeLe16(28, 1u);
  writeLe32(30, 0u);
  writeLe32(34, static_cast<uint32_t>(rowBytes * static_cast<size_t>(height)));
  // 2-entry palette at offset 54..61
  writeLe32(54, 0x00000000u);
  writeLe32(58, 0x00FFFFFFu);
  return bmp;
}

class TxtCoverMetaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fake::reset();
    g_indexMarkedDirty = false;
    g_lastUpdatedRecentPath.clear();
    g_lastUpdatedRecentTitle.clear();
  }
};

}  // namespace

namespace library {
bool markLibraryIndexDirty() {
  g_indexMarkedDirty = true;
  return true;
}
}  // namespace library

void RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string&,
                                  const std::string&) {
  g_lastUpdatedRecentPath = path;
  g_lastUpdatedRecentTitle = title;
}

TEST_F(TxtCoverMetaTest, CanonicalizesPathAndRoundTripsMetaJsonWithCjkAndEscapes) {
  EXPECT_EQ(txtmeta::computePathHashHex("/Books/三体.txt"), txtmeta::computePathHashHex("//Books/./三体.txt"));
  EXPECT_NE(txtmeta::computePathHashHex("/Books/a.txt"), txtmeta::computePathHashHex("/Books/b.txt"));
  EXPECT_EQ(txtmeta::computePathHashHex("/Books/a.txt").size(), 16u);

  const std::string metaPath = txtmeta::getBoundMetaJsonPath("/Books/三体.txt");
  txtmeta::BoundBookMeta in;
  in.title = "三体 \"地球往事\"";
  in.author = "刘慈欣\\大刘";
  in.updatedAt = 1712345678u;
  ASSERT_TRUE(txtmeta::writeMetaJson(metaPath, in));

  txtmeta::BoundBookMeta out;
  ASSERT_TRUE(txtmeta::readMetaJson(metaPath, out));
  EXPECT_EQ(out.title, in.title);
  EXPECT_EQ(out.author, in.author);
  EXPECT_EQ(out.updatedAt, in.updatedAt);
}

TEST_F(TxtCoverMetaTest, FindCompanionCoverImagePrefersHiddenCoverStoreOverSameFolderSidecar) {
  const std::string txtPath = "/Books/novel.txt";
  fake::add(txtPath, "第一章 开始\n正文内容");
  fake::add("/Books/novel.jpg", "same-folder-jpg");
  EXPECT_EQ(Txt::findCompanionCoverImage(txtPath), "/Books/novel.jpg");

  const std::string hiddenBmp = txtmeta::getBoundCoverBmpPath(txtPath);
  fake::add(hiddenBmp, "BM-1bit");
  EXPECT_EQ(Txt::findCompanionCoverImage(txtPath), hiddenBmp);
}

TEST_F(TxtCoverMetaTest, LateCoverBindingInvalidatesThumbAndBookBinWhilePreservingProgressBin) {
  const std::string txtPath = "/Books/十日终焉 - 杀虫队队员.txt";
  const std::string cachePath = "/.crosspoint/epub_test_txt";
  const std::string txtContent = "第一章 轮回\n这是正文内容。\n";
  fake::add(txtPath, txtContent, /*time=*/10);

  auto cache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  ASSERT_TRUE(Txt::buildTxtCache(txtPath, cachePath, cache));
  ASSERT_NE(cache, nullptr);
  EXPECT_EQ(cache->coreMetadata.title, "十日终焉");
  EXPECT_EQ(cache->coreMetadata.author, "杀虫队队员");
  EXPECT_TRUE(cache->coreMetadata.coverItemHref.empty());

  // Simulate user reading progress, rendered sections, and stale thumbnails in cachePath
  fake::add(cachePath + "/progress.bin", "PROGRESS_BYTES");
  fake::add(cachePath + "/sections/0.bin", "SECTION_0");
  fake::add(cachePath + "/thumb_148x222.bmp", "OLD_THUMB");
  fake::add(cachePath + "/cover_legacy_v2.bmp", "OLD_COVER");

  // Cache is initially valid when nothing changed
  EXPECT_TRUE(Txt::validateCache(txtPath, cachePath, txtContent.size()));
  EXPECT_TRUE(Storage.exists((cachePath + "/progress.bin").c_str()));
  EXPECT_TRUE(Storage.exists((cachePath + "/thumb_148x222.bmp").c_str()));

  // Bind a custom cover + custom metadata AFTER the book was previously opened
  const std::string boundBmp = txtmeta::getBoundCoverBmpPath(txtPath);
  fake::add(boundBmp, "BM_CUSTOM_COVER", /*time=*/25);
  txtmeta::BoundBookMeta meta{"十日终焉（精校版）", "杀虫队队员", 1710000000u};
  ASSERT_TRUE(txtmeta::writeMetaJson(txtmeta::getBoundMetaJsonPath(txtPath), meta));

  // validateCache must detect the new cover/metadata, delete stale thumb/cover/cover.stamp, and KEEP progress.bin &
  // sections/0.bin (book.bin stays readable until buildTxtCache refreshes it so cachedSize checks remain intact)
  EXPECT_FALSE(Txt::validateCache(txtPath, cachePath, txtContent.size()));
  EXPECT_TRUE(Storage.exists((cachePath + "/progress.bin").c_str()));
  EXPECT_TRUE(Storage.exists((cachePath + "/sections/0.bin").c_str()));
  EXPECT_FALSE(Storage.exists((cachePath + "/thumb_148x222.bmp").c_str()));
  EXPECT_FALSE(Storage.exists((cachePath + "/cover_legacy_v2.bmp").c_str()));
  EXPECT_FALSE(Storage.exists((cachePath + "/cover.stamp").c_str()));

  // Rebuilding metadata cache with preserveContentCache=true picks up the bound cover and updated title
  // while keeping both progress.bin and sections/0.bin intact
  cache = std::make_unique<BookMetadataCache>(cachePath);
  ASSERT_TRUE(Txt::buildTxtCache(txtPath, cachePath, cache, /*preserveContentCache=*/true));
  EXPECT_EQ(cache->coreMetadata.title, "十日终焉（精校版）");
  EXPECT_EQ(cache->coreMetadata.author, "杀虫队队员");
  EXPECT_EQ(cache->coreMetadata.coverItemHref, boundBmp);
  EXPECT_TRUE(Storage.exists((cachePath + "/progress.bin").c_str()));
  EXPECT_TRUE(Storage.exists((cachePath + "/sections/0.bin").c_str()));
  EXPECT_TRUE(Txt::validateCache(txtPath, cachePath, txtContent.size()));

  // Content edit (different size) MUST invalidate section caches, not just cover thumbnails
  EXPECT_FALSE(Txt::validateCache(txtPath, cachePath, txtContent.size() + 32));
  EXPECT_FALSE(Storage.exists((cachePath + "/sections/0.bin").c_str()));
  EXPECT_TRUE(Storage.exists((cachePath + "/progress.bin").c_str()));
}

TEST_F(TxtCoverMetaTest, BookCoverStoreValidatesBmpPayloadAndMigratesAssetsOnRename) {
  const auto validCanonical =
      buildValid1BitBmp(BookCoverStore::kCanonicalCoverWidth, BookCoverStore::kCanonicalCoverHeight);
  int w = 0, h = 0;
  ASSERT_TRUE(BookCoverStore::validate1BitBmpPayload(validCanonical.data(), validCanonical.size(), &w, &h));
  EXPECT_EQ(w, BookCoverStore::kCanonicalCoverWidth);
  EXPECT_EQ(h, BookCoverStore::kCanonicalCoverHeight);

  // Truncated pixel buffer or non-1bpp header must be rejected
  EXPECT_FALSE(BookCoverStore::validate1BitBmpPayload(validCanonical.data(), 80));
  auto badBpp = validCanonical;
  badBpp[28] = 24;
  EXPECT_FALSE(BookCoverStore::validate1BitBmpPayload(badBpp.data(), badBpp.size()));

  const std::string oldBook = "/Books/draft.txt";
  const std::string newBook = "/Books/final.txt";
  fake::add(oldBook, "第一章 正文");
  const std::string cachePath = Txt::getBookCachePath(oldBook);
  fake::add(cachePath + "/progress.bin", "SAVED_PAGE");
  fake::add(cachePath + "/thumb_102x153.bmp", "STALE_THUMB");

  const std::string customTitle = "定稿小说";
  const std::string customAuthor = "佚名";
  ASSERT_TRUE(BookCoverStore::bindCoverAndMetaFrom1BitBmpBytes(oldBook, validCanonical.data(), validCanonical.size(),
                                                               &customTitle, &customAuthor));
  EXPECT_TRUE(BookCoverStore::hasBoundCover(oldBook));
  EXPECT_TRUE(g_indexMarkedDirty);
  EXPECT_EQ(g_lastUpdatedRecentPath, oldBook);
  EXPECT_EQ(g_lastUpdatedRecentTitle, customTitle);
  // Stale thumbnail purged, reading progress preserved
  EXPECT_FALSE(Storage.exists((cachePath + "/thumb_102x153.bmp").c_str()));
  EXPECT_TRUE(Storage.exists((cachePath + "/progress.bin").c_str()));

  // Rename/move migrates both bound cover BMP and metadata JSON to new path hash
  fake::add(oldBook + ".meta.json", R"({"libby_loan_id":"1"})");
  BookCoverStore::moveBoundAssets(oldBook, newBook);
  EXPECT_FALSE(BookCoverStore::hasBoundCover(oldBook));
  EXPECT_TRUE(BookCoverStore::hasBoundCover(newBook));
  txtmeta::BoundBookMeta movedMeta;
  ASSERT_TRUE(BookCoverStore::readBoundMeta(newBook, movedMeta));
  EXPECT_EQ(movedMeta.title, customTitle);
  EXPECT_EQ(movedMeta.author, customAuthor);
  EXPECT_TRUE(Storage.exists((newBook + ".meta.json").c_str()));

  // Clearing bound cover keeps metadata JSON and reading progress intact
  ASSERT_TRUE(BookCoverStore::clearBoundCover(newBook));
  EXPECT_FALSE(BookCoverStore::hasBoundCover(newBook));
  EXPECT_TRUE(BookCoverStore::readBoundMeta(newBook, movedMeta));

  // Deleting book cleans up remaining bound assets and loan sidecar
  BookCoverStore::removeBoundAssets(newBook);
  EXPECT_FALSE(BookCoverStore::readBoundMeta(newBook, movedMeta));
  EXPECT_FALSE(Storage.exists((newBook + ".meta.json").c_str()));
}
