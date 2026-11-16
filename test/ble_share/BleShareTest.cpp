#include <BleShareProtocol.h>
#include <BleShareSession.h>
#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "network/ProtectedPaths.h"

namespace {

class FakeSessionHost final : public xpbt::SessionHost {
 public:
  bool pairingEnabled = false;
  char pin[xpbt::kPinDigits] = {'4', '8', '2', '9', '1', '7'};
  bool autoAccept = false;
  std::string defaultFolder = "/Books";

  bool hasPaired = false;
  uint8_t pairedPhoneId[xpbt::kIdBytes]{};
  std::string pairedPhoneName;
  uint8_t pairedSharedKey[xpbt::kSha256Bytes]{};

  int offerPendingCalls = 0;
  std::vector<uint8_t> txWire;
  std::map<std::string, std::vector<uint8_t>> committedFiles;
  std::set<std::string> dirs{"/", "/Books"};

  std::string activePath;
  std::vector<uint8_t> activeBytes;
  std::string getActivePath;
  size_t getReadCursor = 0;
  uint8_t rngByte = 0x11;

  void readerIdentity(uint8_t outReaderId[xpbt::kIdBytes], std::string& outReaderName) override {
    for (size_t i = 0; i < xpbt::kIdBytes; ++i) outReaderId[i] = static_cast<uint8_t>(0xA0 + i);
    outReaderName = "CrossPoint X3";
  }

  void fillRandom(uint8_t* out, size_t len) override {
    for (size_t i = 0; i < len; ++i) out[i] = rngByte++;
  }

  bool pairingActive(char outPin[xpbt::kPinDigits]) override {
    if (!pairingEnabled) return false;
    memcpy(outPin, pin, xpbt::kPinDigits);
    return true;
  }

  void onPairSucceeded(const uint8_t phoneId[xpbt::kIdBytes], const std::string& phoneName,
                       const uint8_t sharedKey[xpbt::kSha256Bytes]) override {
    hasPaired = true;
    memcpy(pairedPhoneId, phoneId, xpbt::kIdBytes);
    pairedPhoneName = phoneName;
    memcpy(pairedSharedKey, sharedKey, xpbt::kSha256Bytes);
    pairingEnabled = false;
  }

  bool lookupPairedPhone(const uint8_t phoneId[xpbt::kIdBytes], uint8_t outSharedKey[xpbt::kSha256Bytes],
                         bool& outAutoAccept, std::string& outDefaultFolder) override {
    if (!hasPaired || memcmp(pairedPhoneId, phoneId, xpbt::kIdBytes) != 0) return false;
    memcpy(outSharedKey, pairedSharedKey, xpbt::kSha256Bytes);
    outAutoAccept = autoAccept;
    outDefaultFolder = defaultFolder;
    return true;
  }

  void onOfferPending(const xpbt::BatchOfferInfo&) override { ++offerPendingCalls; }

  bool beginFileWrite(const std::string& destFolder, const std::string& cleanFileName, uint32_t) override {
    activePath = destFolder + "/" + cleanFileName;
    activeBytes.clear();
    return true;
  }

  bool writeFileChunk(const uint8_t* data, size_t len) override {
    activeBytes.insert(activeBytes.end(), data, data + len);
    return true;
  }

  bool finalizeFileWrite(std::string& outSavedPath) override {
    committedFiles[activePath] = activeBytes;
    outSavedPath = activePath;
    activePath.clear();
    activeBytes.clear();
    return true;
  }

  void abortFileWrite() override {
    activePath.clear();
    activeBytes.clear();
  }

  xpbt::FsStatus fsStat(const std::string& path, xpbt::FsStatEntry& out) override {
    if (protectedpaths::isSensitivePath(path.c_str())) return xpbt::FsStatus::Forbidden;
    if (dirs.count(path)) {
      out = {true, true, 0};
      return xpbt::FsStatus::Ok;
    }
    auto it = committedFiles.find(path);
    if (it != committedFiles.end()) {
      out = {true, false, it->second.size()};
      return xpbt::FsStatus::Ok;
    }
    return xpbt::FsStatus::NotFound;
  }

  xpbt::FsStatus fsList(const std::string& path,
                        const std::function<bool(const xpbt::FsDirItem&)>& emit) override {
    if (protectedpaths::isSensitivePath(path.c_str())) return xpbt::FsStatus::Forbidden;
    if (!dirs.count(path)) return xpbt::FsStatus::NotFound;
    const std::string prefix = (path == "/") ? "/" : (path + "/");
    for (const auto& d : dirs) {
      if (d == "/" || d.rfind(prefix, 0) != 0) continue;
      const std::string rem = d.substr(prefix.size());
      if (!rem.empty() && rem.find('/') == std::string::npos) {
        if (!emit({rem, true, 0})) return xpbt::FsStatus::IoError;
      }
    }
    for (const auto& [fp, bytes] : committedFiles) {
      if (fp.rfind(prefix, 0) != 0) continue;
      const std::string rem = fp.substr(prefix.size());
      if (!rem.empty() && rem.find('/') == std::string::npos && !protectedpaths::isSensitivePath(fp.c_str())) {
        if (!emit({rem, false, bytes.size()})) return xpbt::FsStatus::IoError;
      }
    }
    return xpbt::FsStatus::Ok;
  }

  xpbt::FsStatus fsMkdir(const std::string& path) override {
    if (protectedpaths::isSensitivePath(path.c_str())) return xpbt::FsStatus::Forbidden;
    if (dirs.count(path) || committedFiles.count(path)) return xpbt::FsStatus::AlreadyExists;
    dirs.insert(path);
    return xpbt::FsStatus::Ok;
  }

  xpbt::FsStatus fsDelete(const std::string& path, uint8_t mode) override {
    if (protectedpaths::isSensitivePath(path.c_str())) return xpbt::FsStatus::Forbidden;
    if (dirs.count(path)) {
      if (mode == 2) return xpbt::FsStatus::InvalidType;
      dirs.erase(path);
      return xpbt::FsStatus::Ok;
    }
    auto it = committedFiles.find(path);
    if (it == committedFiles.end()) return xpbt::FsStatus::NotFound;
    if (mode == 1) return xpbt::FsStatus::InvalidType;
    committedFiles.erase(it);
    return xpbt::FsStatus::Ok;
  }

  xpbt::FsStatus fsRename(const std::string& srcPath, const std::string& dstPath, bool overwrite) override {
    if (protectedpaths::isSensitivePath(srcPath.c_str()) || protectedpaths::isSensitivePath(dstPath.c_str())) {
      return xpbt::FsStatus::Forbidden;
    }
    auto it = committedFiles.find(srcPath);
    if (it == committedFiles.end()) return xpbt::FsStatus::NotFound;
    if (!overwrite && committedFiles.count(dstPath)) return xpbt::FsStatus::AlreadyExists;
    committedFiles[dstPath] = std::move(it->second);
    committedFiles.erase(it);
    return xpbt::FsStatus::Ok;
  }

  xpbt::FsStatus fsPutBegin(const std::string& path) override {
    if (protectedpaths::isSensitivePath(path.c_str())) return xpbt::FsStatus::Forbidden;
    activePath = path;
    activeBytes.clear();
    return xpbt::FsStatus::Ok;
  }

  bool fsPutChunk(const uint8_t* data, size_t len) override {
    activeBytes.insert(activeBytes.end(), data, data + len);
    return true;
  }

  xpbt::FsStatus fsPutFinish() override {
    committedFiles[activePath] = activeBytes;
    activePath.clear();
    activeBytes.clear();
    return xpbt::FsStatus::Ok;
  }

  void fsPutAbort() override { abortFileWrite(); }

  xpbt::FsStatus fsGetBegin(const std::string& path, uint32_t startOffset, uint64_t& outRemainingBytes) override {
    if (protectedpaths::isSensitivePath(path.c_str())) return xpbt::FsStatus::Forbidden;
    auto it = committedFiles.find(path);
    if (it == committedFiles.end()) return xpbt::FsStatus::NotFound;
    if (startOffset > it->second.size()) return xpbt::FsStatus::IoError;
    getActivePath = path;
    getReadCursor = startOffset;
    outRemainingBytes = it->second.size() - startOffset;
    return xpbt::FsStatus::Ok;
  }

  int fsGetRead(uint8_t* buf, size_t maxLen) override {
    auto it = committedFiles.find(getActivePath);
    if (it == committedFiles.end()) return -1;
    const size_t avail = it->second.size() - getReadCursor;
    const size_t n = std::min(avail, maxLen);
    if (n > 0) {
      memcpy(buf, it->second.data() + getReadCursor, n);
      getReadCursor += n;
    }
    return static_cast<int>(n);
  }

  void fsGetClose() override {
    getActivePath.clear();
    getReadCursor = 0;
  }

  bool sendRawBytes(const uint8_t* data, size_t len) override {
    txWire.insert(txWire.end(), data, data + len);
    return true;
  }

  std::vector<std::pair<xpbt::PacketType, std::vector<uint8_t>>> drainTxPackets() {
    std::vector<std::pair<xpbt::PacketType, std::vector<uint8_t>>> out;
    xpbt::StreamDecoder dec;
    size_t off = 0;
    while (off < txWire.size()) {
      size_t consumed = 0;
      xpbt::PacketView pkt;
      const bool ok = dec.feed(txWire.data() + off, txWire.size() - off, consumed, pkt);
      off += consumed;
      if (ok) {
        out.emplace_back(pkt.type, std::vector<uint8_t>(pkt.payload, pkt.payload + pkt.payloadLength));
        dec.consumeFrame();
      } else if (consumed == 0) {
        break;
      }
    }
    while (true) {
      size_t consumed = 0;
      xpbt::PacketView pkt;
      if (!dec.feed(nullptr, 0, consumed, pkt)) break;
      out.emplace_back(pkt.type, std::vector<uint8_t>(pkt.payload, pkt.payload + pkt.payloadLength));
      dec.consumeFrame();
    }
    txWire.clear();
    return out;
  }
};

void sendFrameFragmented(xpbt::ReceiverSession& session, xpbt::PacketType type, uint32_t seq,
                         const std::vector<uint8_t>& payload, size_t mtuSlice = 17) {
  uint8_t frame[xpbt::kMaxFrameBytes];
  size_t frameLen = 0;
  ASSERT_TRUE(xpbt::encodeFrame(frame, sizeof(frame), type, seq, payload.data(),
                                static_cast<uint16_t>(payload.size()), frameLen));
  for (size_t off = 0; off < frameLen; off += mtuSlice) {
    session.ingestBytes(frame + off, std::min(mtuSlice, frameLen - off));
  }
}

}  // namespace

TEST(BleShareProtocolTest, Sha256AndHmacRfcVectorsMatch) {
  // NIST SHA-256("abc")
  const uint8_t abc[] = {'a', 'b', 'c'};
  uint8_t digest[xpbt::kSha256Bytes];
  xpbt::sha256(abc, sizeof(abc), digest);
  char hex[65];
  xpbt::bytesToHex(digest, sizeof(digest), hex);
  EXPECT_STREQ(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  // RFC 4231 Test Case 2: key = "Jefe", data = "what do ya want for nothing?"
  const char* key = "Jefe";
  const char* msg = "what do ya want for nothing?";
  uint8_t mac[xpbt::kSha256Bytes];
  xpbt::hmacSha256(reinterpret_cast<const uint8_t*>(key), strlen(key), reinterpret_cast<const uint8_t*>(msg),
                   strlen(msg), mac);
  xpbt::bytesToHex(mac, sizeof(mac), hex);
  EXPECT_STREQ(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(BleShareProtocolTest, ProtectedPathsIncludesBleShareStore) {
  EXPECT_TRUE(protectedpaths::isSensitivePath("/.crosspoint/ble_share.json"));
  EXPECT_TRUE(protectedpaths::isSensitivePath("/Books/../.crosspoint/ble_share.json"));
  EXPECT_EQ(xpbt::sanitizeBookFileName("../evil/../../Three Body.epub"), "Three Body.epub");
  EXPECT_EQ(xpbt::sanitizeBookFileName("malware.exe"), "");
  EXPECT_EQ(xpbt::sanitizeDestFolder("/.crosspoint"), "/Books");
  EXPECT_EQ(xpbt::sanitizeDestFolder("/zh/"), "/zh");
}

TEST(BleShareSessionTest, FullPairAuthPromptAndMultiFileChunkedTransfer) {
  FakeSessionHost host;
  xpbt::ReceiverSession session(host);

  uint8_t phoneId[xpbt::kIdBytes];
  uint8_t clientNonce[xpbt::kNonceBytes];
  for (size_t i = 0; i < 16; ++i) {
    phoneId[i] = static_cast<uint8_t>(0x30 + i);
    clientNonce[i] = static_cast<uint8_t>(0x70 + i);
  }

  // 1. Pair with 6-digit PIN over tiny 17-byte fragmented BLE slices.
  host.pairingEnabled = true;
  std::vector<uint8_t> pairHello;
  pairHello.insert(pairHello.end(), phoneId, phoneId + 16);
  pairHello.insert(pairHello.end(), clientNonce, clientNonce + 16);
  const std::string phoneName = "Pixel 10 Pro Fold";
  pairHello.push_back(static_cast<uint8_t>(phoneName.size()));
  pairHello.insert(pairHello.end(), phoneName.begin(), phoneName.end());
  sendFrameFragmented(session, xpbt::PacketType::PairHello, 1, pairHello);

  auto pkts = host.drainTxPackets();
  ASSERT_EQ(pkts.size(), 1u);
  ASSERT_EQ(pkts[0].first, xpbt::PacketType::PairChallenge);
  const uint8_t* readerId = pkts[0].second.data();
  const uint8_t* serverNonce = pkts[0].second.data() + 16;

  uint8_t derivedKey[32];
  uint8_t clientProof[32];
  xpbt::derivePairKey(host.pin, clientNonce, serverNonce, phoneId, readerId, derivedKey);
  xpbt::computeClientPairProof(derivedKey, clientNonce, serverNonce, clientProof);
  sendFrameFragmented(session, xpbt::PacketType::PairVerify, 2, std::vector<uint8_t>(clientProof, clientProof + 32));

  pkts = host.drainTxPackets();
  ASSERT_EQ(pkts.size(), 1u);
  EXPECT_EQ(pkts[0].first, xpbt::PacketType::PairOk);
  EXPECT_TRUE(host.hasPaired);
  EXPECT_EQ(host.pairedPhoneName, "Pixel 10 Pro Fold");

  // 2. Reconnect later using saved sharedKey, offer 2 files, prompt user -> Accept, stream chunks, verify CRC32.
  session.reset();
  std::vector<uint8_t> authHello = pairHello;
  sendFrameFragmented(session, xpbt::PacketType::AuthHello, 1, authHello);
  pkts = host.drainTxPackets();
  ASSERT_EQ(pkts.size(), 1u);
  ASSERT_EQ(pkts[0].first, xpbt::PacketType::AuthChallenge);
  const uint8_t* authServerNonce = pkts[0].second.data() + 16;

  uint8_t authProof[32];
  xpbt::computeAuthProof(host.pairedSharedKey, clientNonce, authServerNonce, phoneId, authProof);
  sendFrameFragmented(session, xpbt::PacketType::AuthVerify, 2, std::vector<uint8_t>(authProof, authProof + 32));
  pkts = host.drainTxPackets();
  ASSERT_EQ(pkts.size(), 1u);
  EXPECT_EQ(pkts[0].first, xpbt::PacketType::AuthOk);

  // Build two synthetic books.
  std::vector<uint8_t> book1(1500);
  for (size_t i = 0; i < book1.size(); ++i) book1[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
  const std::string book2Str = "第一章 灵根育孕源流出\n心性修持大道生\n";
  std::vector<uint8_t> book2(book2Str.begin(), book2Str.end());

  std::vector<uint8_t> offer(10, 0);
  xpbt::writeU16(offer.data(), 2);
  xpbt::writeU64(offer.data() + 2, book1.size() + book2.size());
  const std::string f1Name = "Journey.epub";
  offer.push_back(static_cast<uint8_t>(f1Name.size()));
  offer.insert(offer.end(), f1Name.begin(), f1Name.end());
  offer.push_back(0);  // default destFolder (/Books)
  sendFrameFragmented(session, xpbt::PacketType::BatchOffer, 3, offer);

  EXPECT_EQ(session.state(), xpbt::SessionState::OfferPendingDecision);
  EXPECT_EQ(host.offerPendingCalls, 1);

  // User clicks Accept on reader modal!
  ASSERT_TRUE(session.respondToOffer(true));
  pkts = host.drainTxPackets();
  ASSERT_EQ(pkts.size(), 1u);
  EXPECT_EQ(pkts[0].first, xpbt::PacketType::BatchDecision);
  EXPECT_EQ(pkts[0].second[0], 1u);

  const auto sendOneBook = [&](uint16_t idx, const std::string& name, const std::vector<uint8_t>& data) {
    const uint32_t crc = xpbt::crc32Final(xpbt::crc32Update(xpbt::crc32Init(), data.data(), data.size()));
    std::vector<uint8_t> beginPkt(10, 0);
    xpbt::writeU16(beginPkt.data(), idx);
    xpbt::writeU32(beginPkt.data() + 2, static_cast<uint32_t>(data.size()));
    xpbt::writeU32(beginPkt.data() + 6, crc);
    beginPkt.push_back(static_cast<uint8_t>(name.size()));
    beginPkt.insert(beginPkt.end(), name.begin(), name.end());
    sendFrameFragmented(session, xpbt::PacketType::FileBegin, 10 + idx, beginPkt);
    auto r = host.drainTxPackets();
    ASSERT_EQ(r.size(), 1u);
    ASSERT_EQ(r[0].first, xpbt::PacketType::FileReady);

    for (size_t off = 0; off < data.size(); off += 480) {
      const size_t n = std::min<size_t>(480, data.size() - off);
      std::vector<uint8_t> chunk(4 + n);
      xpbt::writeU32(chunk.data(), static_cast<uint32_t>(off));
      memcpy(chunk.data() + 4, data.data() + off, n);
      sendFrameFragmented(session, xpbt::PacketType::FileChunk, 100 + static_cast<uint32_t>(off), chunk, 23);
    }
    host.drainTxPackets();  // clear intermediate FileProgress ACKs

    std::vector<uint8_t> endPkt(4);
    xpbt::writeU32(endPkt.data(), crc);
    sendFrameFragmented(session, xpbt::PacketType::FileEnd, 900 + idx, endPkt);
    r = host.drainTxPackets();
    ASSERT_EQ(r.size(), 1u);
    ASSERT_EQ(r[0].first, xpbt::PacketType::FileResult);
    EXPECT_EQ(r[0].second[0], static_cast<uint8_t>(xpbt::FileStatus::Ok));
  };

  sendOneBook(0, "Journey.epub", book1);
  sendOneBook(1, "西游记.txt", book2);

  std::vector<uint8_t> donePkt(2);
  xpbt::writeU16(donePkt.data(), 2);
  sendFrameFragmented(session, xpbt::PacketType::BatchDone, 999, donePkt);
  pkts = host.drainTxPackets();
  ASSERT_EQ(pkts.size(), 1u);
  EXPECT_EQ(pkts[0].first, xpbt::PacketType::BatchComplete);
  EXPECT_EQ(session.state(), xpbt::SessionState::Completed);
  EXPECT_EQ(host.committedFiles["/Books/Journey.epub"], book1);
  EXPECT_EQ(host.committedFiles["/Books/西游记.txt"], book2);
}

TEST(BleShareProtocolTest, FsPathSanitizerNormalizesAndRejectsAttacks) {
  EXPECT_EQ(xpbt::sanitizeFsPath("/"), "/");
  EXPECT_EQ(xpbt::sanitizeFsPath("///Books//SciFi/./Dune.epub/"), "/Books/SciFi/Dune.epub");
  EXPECT_EQ(xpbt::sanitizeFsPath("/.sleep/wallpaper.bmp"), "/.sleep/wallpaper.bmp");
  EXPECT_TRUE(xpbt::sanitizeFsPath("/Books/../.crosspoint/wifi.json").empty());
  EXPECT_TRUE(xpbt::sanitizeFsPath("/CROSSP~1/wifi.json").empty());
  EXPECT_TRUE(xpbt::sanitizeFsPath("/Books\\evil.epub").empty());
}

TEST(BleShareSessionTest, AuthenticatedFsRpcListPutRenameGetAndProtect) {
  FakeSessionHost host;
  host.hasPaired = true;
  for (size_t i = 0; i < xpbt::kIdBytes; ++i) host.pairedPhoneId[i] = static_cast<uint8_t>(0x30 + i);
  for (size_t i = 0; i < xpbt::kSha256Bytes; ++i) host.pairedSharedKey[i] = static_cast<uint8_t>(0x70 + i);
  host.committedFiles["/.crosspoint/ble_share.json"] = {1, 2, 3};

  xpbt::ReceiverSession session(host);

  // Authenticate once
  uint8_t clientNonce[xpbt::kNonceBytes];
  for (size_t i = 0; i < xpbt::kNonceBytes; ++i) clientNonce[i] = static_cast<uint8_t>(0x50 + i);
  std::vector<uint8_t> authHello;
  authHello.insert(authHello.end(), host.pairedPhoneId, host.pairedPhoneId + xpbt::kIdBytes);
  authHello.insert(authHello.end(), clientNonce, clientNonce + xpbt::kNonceBytes);
  authHello.push_back(4);
  authHello.insert(authHello.end(), {'F', 'o', 'l', 'd'});
  sendFrameFragmented(session, xpbt::PacketType::AuthHello, 1, authHello);

  auto pkts = host.drainTxPackets();
  ASSERT_EQ(pkts.size(), 1u);
  uint8_t serverNonce[xpbt::kNonceBytes];
  memcpy(serverNonce, pkts[0].second.data() + xpbt::kIdBytes, xpbt::kNonceBytes);
  uint8_t authProof[xpbt::kSha256Bytes];
  xpbt::computeAuthProof(host.pairedSharedKey, clientNonce, serverNonce, host.pairedPhoneId, authProof);
  sendFrameFragmented(session, xpbt::PacketType::AuthVerify, 2,
                      std::vector<uint8_t>(authProof, authProof + xpbt::kSha256Bytes));
  pkts = host.drainTxPackets();
  ASSERT_EQ(pkts.size(), 1u);
  ASSERT_EQ(pkts[0].first, xpbt::PacketType::AuthOk);
  ASSERT_EQ(session.state(), xpbt::SessionState::Authenticated);

  auto makePathPayload = [](const std::string& p) {
    std::vector<uint8_t> b(2 + p.size());
    xpbt::writeU16(b.data(), static_cast<uint16_t>(p.size()));
    memcpy(b.data() + 2, p.data(), p.size());
    return b;
  };

  // 1. Sensitive path access is Forbidden
  sendFrameFragmented(session, xpbt::PacketType::FsStatReq, 3, makePathPayload("/.crosspoint/ble_share.json"));
  pkts = host.drainTxPackets();
  ASSERT_EQ(pkts.size(), 1u);
  EXPECT_EQ(pkts[0].first, xpbt::PacketType::FsStatResp);
  EXPECT_EQ(pkts[0].second[0], static_cast<uint8_t>(xpbt::FsStatus::Forbidden));

  // 2. Mkdir /Archive
  sendFrameFragmented(session, xpbt::PacketType::FsMkdirReq, 4, makePathPayload("/Archive"));
  pkts = host.drainTxPackets();
  ASSERT_EQ(pkts.size(), 1u);
  EXPECT_EQ(pkts[0].first, xpbt::PacketType::FsOpResp);
  EXPECT_EQ(pkts[0].second[0], static_cast<uint8_t>(xpbt::FsStatus::Ok));

  // 3. FsPut a 5,000-byte file (> 8 chunks, testing FsPutAck window + CRC32)
  std::vector<uint8_t> bigPayload(5000);
  for (size_t i = 0; i < bigPayload.size(); ++i) bigPayload[i] = static_cast<uint8_t>((i * 13 + 7) & 0xFF);
  const uint32_t expectedCrc =
      xpbt::crc32Final(xpbt::crc32Update(xpbt::crc32Init(), bigPayload.data(), bigPayload.size()));

  sendFrameFragmented(session, xpbt::PacketType::FsPutBegin, 5, makePathPayload("/Books/Novel.epub"));
  pkts = host.drainTxPackets();
  ASSERT_EQ(pkts.size(), 1u);
  EXPECT_EQ(pkts[0].second[0], static_cast<uint8_t>(xpbt::FsStatus::Ok));

  for (size_t off = 0; off < bigPayload.size(); off += 480) {
    const size_t n = std::min<size_t>(480, bigPayload.size() - off);
    std::vector<uint8_t> chunk(4 + n);
    xpbt::writeU32(chunk.data(), static_cast<uint32_t>(off));
    memcpy(chunk.data() + 4, bigPayload.data() + off, n);
    sendFrameFragmented(session, xpbt::PacketType::FsPutChunk, 10 + static_cast<uint32_t>(off), chunk, 31);
  }
  pkts = host.drainTxPackets();
  ASSERT_FALSE(pkts.empty());
  EXPECT_EQ(pkts[0].first, xpbt::PacketType::FsPutAck);

  std::vector<uint8_t> putEnd(8);
  xpbt::writeU32(putEnd.data(), static_cast<uint32_t>(bigPayload.size()));
  xpbt::writeU32(putEnd.data() + 4, expectedCrc);
  sendFrameFragmented(session, xpbt::PacketType::FsPutEnd, 99, putEnd);
  pkts = host.drainTxPackets();
  ASSERT_EQ(pkts.size(), 1u);
  EXPECT_EQ(pkts[0].first, xpbt::PacketType::FsOpResp);
  EXPECT_EQ(pkts[0].second[0], static_cast<uint8_t>(xpbt::FsStatus::Ok));
  EXPECT_EQ(session.state(), xpbt::SessionState::Authenticated);

  // 4. Instant FsRename /Books/Novel.epub -> /Archive/Novel.epub
  const std::string sPath = "/Books/Novel.epub";
  const std::string dPath = "/Archive/Novel.epub";
  std::vector<uint8_t> ren(1 + 2 + sPath.size() + 2 + dPath.size());
  ren[0] = 1;
  xpbt::writeU16(ren.data() + 1, static_cast<uint16_t>(sPath.size()));
  memcpy(ren.data() + 3, sPath.data(), sPath.size());
  xpbt::writeU16(ren.data() + 3 + sPath.size(), static_cast<uint16_t>(dPath.size()));
  memcpy(ren.data() + 5 + sPath.size(), dPath.data(), dPath.size());
  sendFrameFragmented(session, xpbt::PacketType::FsRenameReq, 100, ren);
  pkts = host.drainTxPackets();
  ASSERT_EQ(pkts.size(), 1u);
  EXPECT_EQ(pkts[0].second[0], static_cast<uint8_t>(xpbt::FsStatus::Ok));
  EXPECT_EQ(host.committedFiles.count("/Books/Novel.epub"), 0u);
  EXPECT_EQ(host.committedFiles["/Archive/Novel.epub"], bigPayload);

  // 5. FsList /Archive
  sendFrameFragmented(session, xpbt::PacketType::FsListReq, 101, makePathPayload("/Archive"));
  pkts = host.drainTxPackets();
  ASSERT_EQ(pkts.size(), 2u);
  EXPECT_EQ(pkts[0].first, xpbt::PacketType::FsListChunk);
  EXPECT_EQ(pkts[1].first, xpbt::PacketType::FsListEnd);
  EXPECT_EQ(pkts[1].second[0], static_cast<uint8_t>(xpbt::FsStatus::Ok));
  EXPECT_EQ(xpbt::readU16(pkts[1].second.data() + 1), 1u);

  // 6. FsGet /Archive/Novel.epub (5,000 B -> 8 chunks in window 1 + FsGetAck -> 3 chunks + FsGetEnd)
  std::vector<uint8_t> getReq(4 + 2 + dPath.size());
  xpbt::writeU32(getReq.data(), 0);
  xpbt::writeU16(getReq.data() + 4, static_cast<uint16_t>(dPath.size()));
  memcpy(getReq.data() + 6, dPath.data(), dPath.size());
  sendFrameFragmented(session, xpbt::PacketType::FsGetBegin, 102, getReq);

  std::vector<uint8_t> downloaded;
  uint32_t endCrc = 0;
  bool gotEnd = false;
  while (!gotEnd) {
    pkts = host.drainTxPackets();
    ASSERT_FALSE(pkts.empty());
    for (const auto& [pt, pl] : pkts) {
      if (pt == xpbt::PacketType::FsStatResp) {
        ASSERT_EQ(pl[0], static_cast<uint8_t>(xpbt::FsStatus::Ok));
        EXPECT_EQ(xpbt::readU64(pl.data() + 2), bigPayload.size());
      } else if (pt == xpbt::PacketType::FsGetChunk) {
        EXPECT_EQ(xpbt::readU32(pl.data()), downloaded.size());
        downloaded.insert(downloaded.end(), pl.begin() + 4, pl.end());
      } else if (pt == xpbt::PacketType::FsGetEnd) {
        EXPECT_EQ(xpbt::readU32(pl.data()), bigPayload.size());
        endCrc = xpbt::readU32(pl.data() + 4);
        gotEnd = true;
      }
    }
    if (!gotEnd) {
      std::vector<uint8_t> ack(4);
      xpbt::writeU32(ack.data(), static_cast<uint32_t>(downloaded.size()));
      sendFrameFragmented(session, xpbt::PacketType::FsGetAck, 103, ack);
    }
  }
  EXPECT_EQ(downloaded, bigPayload);
  EXPECT_EQ(endCrc, expectedCrc);
  EXPECT_EQ(session.state(), xpbt::SessionState::Authenticated);
}
