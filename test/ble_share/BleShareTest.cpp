#include <BleShareProtocol.h>
#include <BleShareSession.h>
#include <gtest/gtest.h>

#include <cstring>
#include <map>
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

  std::string activePath;
  std::vector<uint8_t> activeBytes;
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
