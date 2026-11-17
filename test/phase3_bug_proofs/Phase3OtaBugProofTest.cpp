#include <Arduino.h>
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "FirmwareBoardTag.h"
#include "FirmwareFlasher.h"
#include "HttpDownloader.h"
#include "network/OtaUpdater.h"

namespace {
std::string g_mockReleaseJson;
}  // namespace

bool HttpDownloader::fetchUrl(const std::string&, const DataCallback& onData, const std::string&, const std::string&,
                              const std::vector<Header>&) {
  if (!onData) return false;
  return onData(reinterpret_cast<const uint8_t*>(g_mockReleaseJson.data()), g_mockReleaseJson.size());
}

namespace board_tag {
const char TAG[] = "CROSSPOINT-BOARD-V1:x3;";
const char* boardName() { return "x3"; }
size_t boardNameLen() { return 2; }
void Scanner::feed(const uint8_t*, size_t) {}
}  // namespace board_tag

namespace firmware_flash {
uint16_t runningPartitionChipId() { return 0xFFFF; }
}  // namespace firmware_flash

namespace {

TEST(Phase3OtaBugProofTest, Bug4_SlimVersionIgnoresMatchingReleaseAndAcceptsNewerPatch) {
  // Compiled with -DCROSSPOINT_VERSION="2.0.0-slim"
  ASSERT_STREQ(CROSSPOINT_VERSION, "2.0.0-slim");

  g_mockReleaseJson =
      R"json({"tag_name":"v2.0.0","assets":[{"name":"crosspoint-2.0.0-x3.bin","browser_download_url":"https://example.invalid/fw.bin","size":1048576}]})json";

  OtaUpdater updater;
  ASSERT_EQ(updater.checkForUpdate(), OtaUpdater::OK);
  EXPECT_EQ(updater.getLatestVersion(), "v2.0.0");
  EXPECT_FALSE(updater.isUpdateNewer());

  g_mockReleaseJson =
      R"json({"tag_name":"v2.0.1","assets":[{"name":"crosspoint-2.0.1-x3.bin","browser_download_url":"https://example.invalid/fw.bin","size":1048576}]})json";
  ASSERT_EQ(updater.checkForUpdate(), OtaUpdater::OK);
  EXPECT_EQ(updater.getLatestVersion(), "v2.0.1");
  EXPECT_TRUE(updater.isUpdateNewer());
}

}  // namespace
