#include "BleShareStore.h"

#include <CredentialIntegrity.h>
#include <FsHelpers.h>
#include <ObfuscationUtils.h>

#include <cstring>

#include "network/ProtectedPaths.h"

void BleShareStore::toJson(JsonDocument& doc) const {
  doc["enabled"] = enabled_;
  doc["autoAccept"] = autoAccept_;
  doc["readerId"] = readerIdHex_;
  doc["pairedPhoneId"] = pairedPhoneIdHex_;
  doc["pairedPhoneName"] = pairedPhoneName_;
  doc["sharedKey_obf"] = obfuscation::obfuscateToBase64(sharedKeyHex_);
  doc["sharedKey_crc32"] = credential_integrity::crc32(sharedKeyHex_);
  doc["destFolder"] = destFolder_;
}

bool BleShareStore::fromJson(JsonVariantConst doc) {
  enabled_ = doc["enabled"] | true;
  autoAccept_ = doc["autoAccept"] | false;
  readerIdHex_ = doc["readerId"] | "";
  pairedPhoneIdHex_ = doc["pairedPhoneId"] | "";
  pairedPhoneName_ = doc["pairedPhoneName"] | "";
  setDestFolder(doc["destFolder"] | "/Books");

  const char* obf = doc["sharedKey_obf"] | "";
  if (obf[0] != '\0') {
    bool ok = false;
    std::string decoded = obfuscation::deobfuscateFromBase64(obf, &ok);
    if (ok && doc["sharedKey_crc32"].is<uint32_t>()) {
      const uint32_t expectedCrc = doc["sharedKey_crc32"].as<uint32_t>();
      if (!credential_integrity::validate(decoded, decoded.size(), expectedCrc)) {
        ok = false;
      }
    }
    sharedKeyHex_ = ok ? std::move(decoded) : std::string();
  } else {
    sharedKeyHex_.clear();
  }

  if (pairedPhoneIdHex_.size() != xpbt::kIdBytes * 2 || sharedKeyHex_.size() != xpbt::kSha256Bytes * 2) {
    pairedPhoneIdHex_.clear();
    pairedPhoneName_.clear();
    sharedKeyHex_.clear();
  }
  return true;
}

void BleShareStore::setDestFolder(const std::string& folder) {
  if (folder.empty() || folder.find("..") != std::string::npos) {
    destFolder_ = "/Books";
    return;
  }
  const std::string raw = (folder[0] == '/') ? folder : ("/" + folder);
  const std::string stripped = FsHelpers::normalisePath(raw);
  std::string norm = stripped.empty() ? "/" : ("/" + stripped);
  while (norm.size() > 1 && norm.back() == '/') {
    norm.pop_back();
  }
  if (norm.size() <= 1 || norm[0] != '/' || norm.find("/.") != std::string::npos ||
      norm.find('\\') != std::string::npos || protectedpaths::isSensitivePath(norm.c_str())) {
    destFolder_ = "/Books";
    return;
  }
  destFolder_ = std::move(norm);
}

void BleShareStore::ensureReaderId(const uint8_t fallbackRandom[xpbt::kIdBytes]) {
  uint8_t dummy[xpbt::kIdBytes];
  if (xpbt::hexToBytes(readerIdHex_, dummy, sizeof(dummy))) return;
  char hex[xpbt::kIdBytes * 2 + 1];
  xpbt::bytesToHex(fallbackRandom, xpbt::kIdBytes, hex);
  readerIdHex_ = hex;
  saveToFile();
}

bool BleShareStore::getReaderIdBytes(uint8_t out[xpbt::kIdBytes]) const {
  return xpbt::hexToBytes(readerIdHex_, out, xpbt::kIdBytes);
}

void BleShareStore::setPairedPhone(const uint8_t phoneId[xpbt::kIdBytes], const std::string& phoneName,
                                   const uint8_t sharedKey[xpbt::kSha256Bytes]) {
  char idHex[xpbt::kIdBytes * 2 + 1];
  char keyHex[xpbt::kSha256Bytes * 2 + 1];
  xpbt::bytesToHex(phoneId, xpbt::kIdBytes, idHex);
  xpbt::bytesToHex(sharedKey, xpbt::kSha256Bytes, keyHex);
  pairedPhoneIdHex_ = idHex;
  pairedPhoneName_ = phoneName.empty() ? "Android Phone" : phoneName;
  sharedKeyHex_ = keyHex;
  enabled_ = true;
  saveToFile();
}

bool BleShareStore::matchPairedPhone(const uint8_t phoneId[xpbt::kIdBytes],
                                     uint8_t outSharedKey[xpbt::kSha256Bytes]) const {
  if (!isPaired()) return false;
  uint8_t storedId[xpbt::kIdBytes];
  if (!xpbt::hexToBytes(pairedPhoneIdHex_, storedId, sizeof(storedId))) return false;
  if (memcmp(storedId, phoneId, sizeof(storedId)) != 0) return false;
  return xpbt::hexToBytes(sharedKeyHex_, outSharedKey, xpbt::kSha256Bytes);
}

void BleShareStore::clearPairing() {
  pairedPhoneIdHex_.clear();
  pairedPhoneName_.clear();
  sharedKeyHex_.clear();
  saveToFile();
}
