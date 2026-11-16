#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "BleShareProtocol.h"

namespace xpbt {

struct FsStatEntry {
  bool exists = false;
  bool isDir = false;
  uint64_t sizeBytes = 0;
};

struct FsDirItem {
  std::string name;
  bool isDir = false;
  uint64_t sizeBytes = 0;
};

struct BatchOfferInfo {
  std::string phoneName;
  std::string firstFileName;
  std::string destFolder;
  uint16_t fileCount = 0;
  uint64_t totalBytes = 0;
};

struct ActiveFileProgress {
  uint16_t fileIndex = 0;
  uint16_t fileCount = 0;
  std::string fileName;
  uint32_t receivedBytes = 0;
  uint32_t fileSize = 0;
  uint64_t batchReceivedBytes = 0;
  uint64_t batchTotalBytes = 0;
};

enum class SessionState : uint8_t {
  Idle,
  PairWaitVerify,
  AuthWaitVerify,
  Authenticated,
  OfferPendingDecision,
  ReadyForFiles,
  ReceivingFile,
  FsPutting,
  FsGetting,
  Completed,
  Deferred,
  Error,
};

// Host callbacks for storage, credentials, randomness, and outgoing wire bytes.
class SessionHost {
 public:
  virtual ~SessionHost() = default;

  virtual void readerIdentity(uint8_t outReaderId[kIdBytes], std::string& outReaderName) = 0;
  virtual void fillRandom(uint8_t* out, size_t len) = 0;

  // Pairing configuration: returns true if the reader currently accepts new pairing with `outPin` (6 ASCII digits).
  virtual bool pairingActive(char outPin[kPinDigits]) = 0;
  virtual void onPairSucceeded(const uint8_t phoneId[kIdBytes], const std::string& phoneName,
                               const uint8_t sharedKey[kSha256Bytes]) = 0;

  // Auth lookup for an already-paired phone.
  virtual bool lookupPairedPhone(const uint8_t phoneId[kIdBytes], uint8_t outSharedKey[kSha256Bytes],
                                 bool& outAutoAccept, std::string& outDefaultFolder) = 0;

  // Called when an authenticated phone offers a batch and auto-accept is false.
  virtual void onOfferPending(const BatchOfferInfo& offer) = 0;

  // Storage hooks for streaming incoming files safely to a temp file, then committing on CRC pass.
  virtual bool beginFileWrite(const std::string& destFolder, const std::string& cleanFileName, uint32_t expectedSize) = 0;
  virtual bool writeFileChunk(const uint8_t* data, size_t len) = 0;
  virtual bool finalizeFileWrite(std::string& outSavedPath) = 0;
  virtual void abortFileWrite() = 0;

  // Authenticated file-system RPC hooks (used by the localhost FTP-over-BLE bridge).
  virtual FsStatus fsStat(const std::string& /*path*/, FsStatEntry& /*out*/) { return FsStatus::Forbidden; }
  virtual FsStatus fsList(const std::string& /*path*/, const std::function<bool(const FsDirItem&)>& /*emit*/) {
    return FsStatus::Forbidden;
  }
  virtual FsStatus fsMkdir(const std::string& /*path*/) { return FsStatus::Forbidden; }
  virtual FsStatus fsDelete(const std::string& /*path*/, uint8_t /*mode*/) { return FsStatus::Forbidden; }
  virtual FsStatus fsRename(const std::string& /*srcPath*/, const std::string& /*dstPath*/, bool /*overwrite*/) {
    return FsStatus::Forbidden;
  }
  virtual FsStatus fsPutBegin(const std::string& /*path*/) { return FsStatus::Forbidden; }
  virtual bool fsPutChunk(const uint8_t* /*data*/, size_t /*len*/) { return false; }
  virtual FsStatus fsPutFinish() { return FsStatus::Forbidden; }
  virtual void fsPutAbort() {}
  virtual FsStatus fsGetBegin(const std::string& /*path*/, uint32_t /*startOffset*/, uint64_t& /*outRemainingBytes*/) {
    return FsStatus::Forbidden;
  }
  virtual int fsGetRead(uint8_t* /*buf*/, size_t /*maxLen*/) { return -1; }
  virtual void fsGetClose() {}

  // Outgoing packet bytes to write to BLE TX characteristic or TCP socket.
  virtual bool sendRawBytes(const uint8_t* data, size_t len) = 0;
};

class ReceiverSession {
 public:
  static constexpr uint16_t kAckEveryChunks = 8;

  explicit ReceiverSession(SessionHost& host) : host_(host) {}

  void reset();
  void onDisconnected();
  void ingestBytes(const uint8_t* data, size_t len);

  // User responded to the incoming prompt (true = Accept, false = Later/Reject).
  bool respondToOffer(bool accept);

  SessionState state() const { return state_; }
  const BatchOfferInfo& offer() const { return offer_; }
  const ActiveFileProgress& progress() const { return progress_; }
  const std::vector<std::string>& savedPaths() const { return savedPaths_; }
  const std::string& errorMessage() const { return errorMessage_; }
  const std::string& peerName() const { return peerName_; }
  uint16_t filesSaved() const { return filesSaved_; }

 private:
  void handlePacket(const PacketView& pkt);
  void handlePairHello(const PacketView& pkt);
  void handlePairVerify(const PacketView& pkt);
  void handleAuthHello(const PacketView& pkt);
  void handleAuthVerify(const PacketView& pkt);
  void handleBatchOffer(const PacketView& pkt);
  void handleFileBegin(const PacketView& pkt);
  void handleFileChunk(const PacketView& pkt);
  void handleFileEnd(const PacketView& pkt);
  void handleBatchDone(const PacketView& pkt);

  void handleFsStat(const PacketView& pkt);
  void handleFsList(const PacketView& pkt);
  void handleFsMkdir(const PacketView& pkt);
  void handleFsDelete(const PacketView& pkt);
  void handleFsRename(const PacketView& pkt);
  void handleFsPutBegin(const PacketView& pkt);
  void handleFsPutChunk(const PacketView& pkt);
  void handleFsPutEnd(const PacketView& pkt);
  void handleFsGetBegin(const PacketView& pkt);
  void handleFsGetAck(const PacketView& pkt);
  void handleFsAbort();
  void pumpFsGetWindow();
  void sendFsOpResp(FsStatus status, const char* msg = "");

  bool sendPacket(PacketType type, const void* payload, uint16_t payloadLen);
  void sendError(ErrorCode code, const char* msg);

  SessionHost& host_;
  StreamDecoder decoder_;
  uint8_t txFrame_[kMaxFrameBytes]{};
  SessionState state_ = SessionState::Idle;
  uint32_t txSeq_ = 0;

  uint8_t readerId_[kIdBytes]{};
  std::string readerName_;
  uint8_t phoneId_[kIdBytes]{};
  std::string peerName_;
  uint8_t clientNonce_[kNonceBytes]{};
  uint8_t serverNonce_[kNonceBytes]{};
  uint8_t sessionKey_[kSha256Bytes]{};
  bool autoAccept_ = false;
  std::string defaultFolder_ = "/Books";

  BatchOfferInfo offer_;
  ActiveFileProgress progress_;
  uint32_t expectedFileCrc_ = 0;
  uint32_t runningFileCrc_ = 0;
  uint16_t chunksSinceAck_ = 0;
  uint32_t fsStreamOffset_ = 0;
  uint32_t fsGetSentBytes_ = 0;
  uint64_t fsGetRemainingBytes_ = 0;
  uint16_t filesSaved_ = 0;
  std::vector<std::string> savedPaths_;
  std::string errorMessage_;
};

}  // namespace xpbt
