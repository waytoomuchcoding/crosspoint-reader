#pragma once

#include <ArduinoJson.h>
#include <BleShareProtocol.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

class BleShareStore : public PersistableStore<BleShareStore> {
 private:
  bool enabled_ = true;
  bool autoAccept_ = false;
  std::string readerIdHex_;
  std::string pairedPhoneIdHex_;
  std::string pairedPhoneName_;
  std::string sharedKeyHex_;
  std::string destFolder_ = "/Books";

  BleShareStore() = default;
  ~BleShareStore() = default;

  friend class PersistableStore<BleShareStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/ble_share.json"; }

  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool isEnabled() const { return enabled_; }
  void setEnabled(bool enabled) { enabled_ = enabled; }

  bool isAutoAccept() const { return autoAccept_; }
  void setAutoAccept(bool autoAccept) { autoAccept_ = autoAccept; }

  const std::string& getDestFolder() const { return destFolder_; }
  void setDestFolder(const std::string& folder);

  void ensureReaderId(const uint8_t fallbackRandom[xpbt::kIdBytes]);
  bool getReaderIdBytes(uint8_t out[xpbt::kIdBytes]) const;

  bool isPaired() const {
    return pairedPhoneIdHex_.size() == xpbt::kIdBytes * 2 && sharedKeyHex_.size() == xpbt::kSha256Bytes * 2;
  }

  const std::string& getPairedPhoneName() const { return pairedPhoneName_; }
  const std::string& getPairedPhoneIdHex() const { return pairedPhoneIdHex_; }

  void setPairedPhone(const uint8_t phoneId[xpbt::kIdBytes], const std::string& phoneName,
                      const uint8_t sharedKey[xpbt::kSha256Bytes]);
  bool matchPairedPhone(const uint8_t phoneId[xpbt::kIdBytes], uint8_t outSharedKey[xpbt::kSha256Bytes]) const;
  void clearPairing();
};

#define BLESHARE_STORE BleShareStore::getInstance()
