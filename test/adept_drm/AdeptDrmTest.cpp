#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "HostSyncFakes.h"
#include "network/AdeptFulfiller.h"
#include "util/Iso8601.h"

TEST(AdeptDrmTest, Iso8601UtcAndOffsetAndValidation) {
  EXPECT_EQ(iso8601::parseUtc("1970-01-01T00:00:00Z"), 0LL);
  EXPECT_EQ(iso8601::parseUtc("2026-11-23T18:30:00Z"), 1795458600LL);
  EXPECT_EQ(iso8601::parseUtc("2026-11-23T20:30:00+02:00"), 1795458600LL);
  EXPECT_EQ(iso8601::parseUtc("2026-11-23T13:30:00-05:00"), 1795458600LL);
  EXPECT_EQ(iso8601::parseUtc("2024-02-29T00:00:00Z"), 1709164800LL);  // Leap year valid
  EXPECT_EQ(iso8601::parseUtc("2026-02-29T00:00:00Z"), 0LL);           // Non-leap Feb 29 rejected
  EXPECT_EQ(iso8601::parseUtc("2026-13-01T00:00:00Z"), 0LL);           // Invalid month rejected
  EXPECT_EQ(iso8601::parseUtc("2026-11-23T18:30:00Zjunk"), 0LL);       // Trailing garbage rejected
  EXPECT_EQ(iso8601::parseUtc("invalid"), 0LL);
}

TEST(AdeptDrmTest, DetectsAcsmFileAndContent) {
  EXPECT_TRUE(adept::isAcsmFile("/Libby/loan.ACSM"));
  EXPECT_FALSE(adept::isAcsmFile("/Libby/loan.epub"));
  EXPECT_TRUE(adept::isAcsmContent("<?xml version=\"1.0\"?>\n<fulfillmentToken xmlns=\"http://ns.adobe.com/adept\">"));
  EXPECT_FALSE(adept::isAcsmContent("PK\x03\x04not-xml"));
}

TEST(AdeptDrmTest, CanonicalXmlSha1IgnoresTopLevelSignatureAndBuildsSignedFulfill) {
  const char* sampleAcsm =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<fulfillmentToken fulfillmentType=\"loan\" auth=\"user\" xmlns=\"http://ns.adobe.com/adept\">"
      "<distributor>urn:uuid:9cb786e8-586a-4950-8901-fff8d2ee6025</distributor>"
      "<operatorURL>https://acs.ebookcentral.com/fulfillment</operatorURL>"
      "<transaction>LIBBY-LOAN-12345</transaction>"
      "<expiration>2026-11-30T12:00:00Z</expiration>"
      "<resourceItemInfo>"
      "<resource>urn:uuid:11111111-2222-3333-4444-555555555555</resource>"
      "</resourceItemInfo>"
      "<hmac>dGVzdGhtYWM=</hmac>"
      "<signature>cHJlZXhpc3RpbmdTaWduYXR1cmU=</signature>"
      "</fulfillmentToken>";

  const char* sampleAcsmDifferentSig =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<fulfillmentToken fulfillmentType=\"loan\" auth=\"user\" xmlns=\"http://ns.adobe.com/adept\">"
      "<distributor>urn:uuid:9cb786e8-586a-4950-8901-fff8d2ee6025</distributor>"
      "<operatorURL>https://acs.ebookcentral.com/fulfillment</operatorURL>"
      "<transaction>LIBBY-LOAN-12345</transaction>"
      "<expiration>2026-11-30T12:00:00Z</expiration>"
      "<resourceItemInfo>"
      "<resource>urn:uuid:11111111-2222-3333-4444-555555555555</resource>"
      "</resourceItemInfo>"
      "<hmac>dGVzdGhtYWM=</hmac>"
      "<signature>OTHER_SIGNATURE_IGNORED</signature>"
      "</fulfillmentToken>";

  uint8_t digest1[20] = {0};
  uint8_t digest2[20] = {0};
  ASSERT_TRUE(adept::computeAdeptXmlSha1(sampleAcsm, digest1));
  ASSERT_TRUE(adept::computeAdeptXmlSha1(sampleAcsmDifferentSig, digest2));
  EXPECT_EQ(std::memcmp(digest1, digest2, 20), 0);

  adept::DeviceCredentials creds;
  creds.userUrn = "urn:uuid:aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
  creds.deviceUrn = "urn:uuid:12345678-1234-1234-1234-1234567890ab";
  creds.deviceType = "standalone";
  creds.privateKeyDer = {0x30, 0x82, 0x01, 0x00};

  std::string operatorUrl;
  std::string fulfillXml;
  std::string errorOut;
  ASSERT_TRUE(adept::buildSignedFulfillRequest(sampleAcsm, creds, operatorUrl, fulfillXml, errorOut));
  EXPECT_EQ(operatorUrl, "https://acs.ebookcentral.com/fulfillment/Fulfill");
  EXPECT_NE(fulfillXml.find("<fulfill xmlns=\"http://ns.adobe.com/adept\">"), std::string::npos);
  EXPECT_NE(fulfillXml.find("<signature>"), std::string::npos);
  EXPECT_NE(fulfillXml.find("<device>urn:uuid:12345678-1234-1234-1234-1234567890ab</device>"), std::string::npos);
}

TEST(AdeptDrmTest, Pkcs1v15AesKeyExtractionValidatesPaddingStrictly) {
  std::vector<uint8_t> block(128, 0x5A);
  block[0] = 0x00;
  block[1] = 0x02;
  block[111] = 0x00;
  for (int i = 0; i < 16; ++i) {
    block[112 + i] = static_cast<uint8_t>(0x20 + i);
  }

  uint8_t key[16] = {0};
  ASSERT_TRUE(adept::extractPkcs1v15AesKey(block.data(), block.size(), key));
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(key[i], static_cast<uint8_t>(0x20 + i));
  }

  // Leading 0x00 stripped by bignum implementation (plainLen == 127 starting at 0x02) must also succeed
  std::memset(key, 0, sizeof(key));
  ASSERT_TRUE(adept::extractPkcs1v15AesKey(block.data() + 1, block.size() - 1, key));
  EXPECT_EQ(key[0], 0x20);

  // Wrong block type (0x01 instead of 0x02) -> rejected
  auto badType = block;
  badType[1] = 0x01;
  EXPECT_FALSE(adept::extractPkcs1v15AesKey(badType.data(), badType.size(), key));

  // Short PS (< 8 non-zero bytes before 0x00 delimiter) -> rejected
  auto shortPs = block;
  shortPs[5] = 0x00;
  EXPECT_FALSE(adept::extractPkcs1v15AesKey(shortPs.data(), shortPs.size(), key));

  // Payload length != 16 bytes -> rejected
  auto wrongPayloadLen = block;
  wrongPayloadLen[111] = 0x5A;
  wrongPayloadLen[110] = 0x00;
  EXPECT_FALSE(adept::extractPkcs1v15AesKey(wrongPayloadLen.data(), wrongPayloadLen.size(), key));

  // Full unwrapEncryptedBookKey path
  std::string err;
  const std::vector<uint8_t> dummyKey = {0x30, 0x82, 0x01, 0x00};
  ASSERT_TRUE(adept::unwrapEncryptedBookKey("EBESExQVFhcYGRobHB0eHw==", dummyKey, key, err));
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(key[i], static_cast<uint8_t>(0x10 + i));
  }
}

TEST(AdeptDrmTest, ParsesFulfillmentResultAndErrorXml) {
  adept::FulfillmentReply reply;
  std::string err;
  EXPECT_FALSE(adept::parseFulfillmentResultXml(
      "<error xmlns=\"http://ns.adobe.com/adept\" data=\"E_ADEPT_DOCUMENT_EXPIRED Token expired\"/>", reply, err));
  EXPECT_NE(err.find("E_ADEPT_DOCUMENT_EXPIRED"), std::string::npos);

  const char* okXml =
      "<envelope xmlns=\"http://ns.adobe.com/adept\">"
      "<fulfillmentResult>"
      "<resourceItemInfo>"
      "<resource>urn:uuid:11111111-2222-3333-4444-555555555555</resource>"
      "<src>https://content.overdrive.com/books/sample.epub</src>"
      "<licenseToken>"
      "<permissions><display><until>2026-11-23T18:30:00Z</until></display></permissions>"
      "<encryptedKey>dGVzdEtleQ==</encryptedKey>"
      "</licenseToken>"
      "</resourceItemInfo>"
      "</fulfillmentResult>"
      "</envelope>";
  err.clear();
  reply = {};
  ASSERT_TRUE(adept::parseFulfillmentResultXml(okXml, reply, err));
  EXPECT_EQ(reply.downloadUrl, "https://content.overdrive.com/books/sample.epub");
  EXPECT_EQ(reply.encryptedKeyB64, "dGVzdEtleQ==");
  EXPECT_EQ(reply.expiresAt, 1795458600LL);
}
