#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace adept {

struct DeviceCredentials {
  std::string userUrn;  // e.g. "urn:uuid:..."
  std::string deviceUrn;
  std::string deviceType = "standalone";
  std::vector<uint8_t> privateKeyDer;  // PKCS#8 or PKCS#1 DER private key
};

struct FulfillmentReply {
  std::string downloadUrl;      // <resourceItemInfo><src>
  std::string encryptedKeyB64;  // <licenseToken><encryptedKey>
  int64_t expiresAt = 0;        // Unix epoch seconds from <until> (0 = no expiry)
  std::string errorCode;        // <error xmlns="http://ns.adobe.com/adept" data="..."/>
};

// Returns true if `path` ends with `.acsm` (case-insensitive) or its contents start with `<fulfillmentToken`.
bool isAcsmFile(const std::string& path);
bool isAcsmContent(std::string_view content);

// Computes the canonical Adobe ADEPT SHA-1 digest over an XML fragment (excluding top-level <signature>).
bool computeAdeptXmlSha1(std::string_view xmlFragment, uint8_t sha1Out[20]);

// Builds and signs the `<fulfill>` XML request body from an `.acsm` `<fulfillmentToken>` XML document.
// Extracts the target `operatorUrlOut` (ending in `/Fulfill`).
bool buildSignedFulfillRequest(std::string_view acsmXml, const DeviceCredentials& creds, std::string& operatorUrlOut,
                               std::string& signedFulfillXmlOut, std::string& errOut);

// Parses the ACS `<fulfillmentResult>` (or `<error>`) XML response.
bool parseFulfillmentResultXml(std::string_view responseXml, FulfillmentReply& replyOut, std::string& errOut);

// Parses an ISO-8601 UTC timestamp ("YYYY-MM-DDTHH:MM:SSZ" or with +/-HH:MM offset) into Unix epoch seconds.
int64_t parseIso8601Timestamp(std::string_view iso);

// Validates and strips PKCS#1 v1.5 type-2 encryption block padding (`00 02 [>=8 non-zero bytes] 00 [16-byte key]`).
bool extractPkcs1v15AesKey(const uint8_t* plain, size_t plainLen, uint8_t aesKeyOut[16]);

// Unwraps a base64 RSA-encrypted 16-byte AES content key using the device's private key.
bool unwrapEncryptedBookKey(std::string_view encryptedKeyB64, const std::vector<uint8_t>& privateKeyDer,
                            uint8_t aesKeyOut[16], std::string& errOut);

// Loads activated Adobe ADEPT device credentials from SD card (`/.crosspoint/adept.json`,
// `/.crosspoint/plugins/protected-content/*`, `/plugins/protected-content/*`, or `/.crosspoint/adept/*`).
bool loadDeviceCredentials(DeviceCredentials& credsOut, std::string& errOut);

// End-to-end on-device `.acsm` fulfillment:
// 1. Loads device credentials from SD
// 2. Parses `acsmXml` and builds signed `<fulfill>` XML
// 3. POSTs `<fulfill>` to `<operatorURL>/Fulfill`
// 4. Downloads the encrypted `.epub` to `destEpubPath`
// 5. Unwraps `<encryptedKey>` and writes `<destEpubPath>.key` via `bookkey::write()`
bool fulfillAcsmContentToEpub(std::string_view acsmXml, const std::string& destEpubPath, std::string& errOut,
                              std::function<void(size_t, size_t)> progressCb = nullptr);

// Fulfills an `.acsm` file on SD card into `destEpubPath` (if `destEpubPath` is empty, replaces `.acsm` suffix with
// `.epub` and removes the source `.acsm` ticket on success).
bool fulfillAcsmFile(const std::string& acsmPath, std::string& finalEpubPathOut, std::string& errOut,
                     std::function<void(size_t, size_t)> progressCb = nullptr);

}  // namespace adept
