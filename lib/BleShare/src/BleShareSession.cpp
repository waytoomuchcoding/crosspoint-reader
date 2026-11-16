#include "BleShareSession.h"

#include <algorithm>
#include <cstring>

namespace xpbt {
namespace {

bool readLenPrefixedString(const uint8_t* data, const size_t avail, size_t& offset, const size_t maxAllowed,
                           std::string& out) {
  if (offset >= avail) return false;
  const uint8_t len = data[offset++];
  if (len > maxAllowed || offset + len > avail) return false;
  out.assign(reinterpret_cast<const char*>(data + offset), len);
  offset += len;
  return true;
}

bool readU16LenString(const uint8_t* data, const size_t avail, size_t& offset, const size_t maxAllowed,
                      std::string& out) {
  if (offset + 2 > avail) return false;
  const uint16_t len = readU16(data + offset);
  offset += 2;
  if (len > maxAllowed || offset + len > avail) return false;
  out.assign(reinterpret_cast<const char*>(data + offset), len);
  offset += len;
  return true;
}

}  // namespace

void ReceiverSession::reset() {
  if (state_ == SessionState::ReceivingFile) {
    host_.abortFileWrite();
  } else if (state_ == SessionState::FsPutting) {
    host_.fsPutAbort();
  } else if (state_ == SessionState::FsGetting) {
    host_.fsGetClose();
  }
  decoder_.reset();
  state_ = SessionState::Idle;
  txSeq_ = 0;
  memset(phoneId_, 0, sizeof(phoneId_));
  peerName_.clear();
  memset(clientNonce_, 0, sizeof(clientNonce_));
  memset(serverNonce_, 0, sizeof(serverNonce_));
  memset(sessionKey_, 0, sizeof(sessionKey_));
  autoAccept_ = false;
  defaultFolder_ = "/Books";
  offer_ = BatchOfferInfo{};
  progress_ = ActiveFileProgress{};
  expectedFileCrc_ = 0;
  runningFileCrc_ = 0;
  chunksSinceAck_ = 0;
  fsStreamOffset_ = 0;
  fsGetSentBytes_ = 0;
  fsGetRemainingBytes_ = 0;
  filesSaved_ = 0;
  savedPaths_.clear();
  errorMessage_.clear();
}

void ReceiverSession::onDisconnected() {
  if (state_ == SessionState::ReceivingFile) {
    host_.abortFileWrite();
    state_ = SessionState::Error;
    errorMessage_ = "Connection closed during transfer";
    return;
  }
  if (state_ == SessionState::FsPutting) {
    host_.fsPutAbort();
  } else if (state_ == SessionState::FsGetting) {
    host_.fsGetClose();
  }
  if (state_ != SessionState::Completed && state_ != SessionState::Deferred) {
    reset();
  } else {
    decoder_.reset();
  }
}

void ReceiverSession::ingestBytes(const uint8_t* data, const size_t len) {
  size_t offset = 0;
  while (offset < len) {
    size_t consumed = 0;
    PacketView pkt;
    const bool ready = decoder_.feed(data + offset, len - offset, consumed, pkt);
    offset += consumed;
    if (ready) {
      handlePacket(pkt);
      decoder_.consumeFrame();
    } else if (consumed == 0) {
      break;
    }
  }
  // Drain any additional complete frames already buffered inside decoder_.
  while (true) {
    size_t consumed = 0;
    PacketView pkt;
    if (!decoder_.feed(nullptr, 0, consumed, pkt)) break;
    handlePacket(pkt);
    decoder_.consumeFrame();
  }
}

bool ReceiverSession::respondToOffer(const bool accept) {
  if (state_ != SessionState::OfferPendingDecision) return false;
  uint8_t payload[3];
  payload[0] = accept ? 1 : 0;
  writeU16(payload + 1, kDefaultChunkDataBytes);
  if (!sendPacket(PacketType::BatchDecision, payload, sizeof(payload))) {
    state_ = SessionState::Error;
    errorMessage_ = "Failed to send response";
    return false;
  }
  if (accept) {
    state_ = SessionState::ReadyForFiles;
  } else {
    state_ = SessionState::Deferred;
  }
  return true;
}

void ReceiverSession::handlePacket(const PacketView& pkt) {
  switch (pkt.type) {
    case PacketType::PairHello:
      handlePairHello(pkt);
      break;
    case PacketType::PairVerify:
      handlePairVerify(pkt);
      break;
    case PacketType::AuthHello:
      handleAuthHello(pkt);
      break;
    case PacketType::AuthVerify:
      handleAuthVerify(pkt);
      break;
    case PacketType::BatchOffer:
      handleBatchOffer(pkt);
      break;
    case PacketType::FileBegin:
      handleFileBegin(pkt);
      break;
    case PacketType::FileChunk:
      handleFileChunk(pkt);
      break;
    case PacketType::FileEnd:
      handleFileEnd(pkt);
      break;
    case PacketType::BatchDone:
      handleBatchDone(pkt);
      break;
    case PacketType::FsStatReq:
      handleFsStat(pkt);
      break;
    case PacketType::FsListReq:
      handleFsList(pkt);
      break;
    case PacketType::FsMkdirReq:
      handleFsMkdir(pkt);
      break;
    case PacketType::FsDeleteReq:
      handleFsDelete(pkt);
      break;
    case PacketType::FsRenameReq:
      handleFsRename(pkt);
      break;
    case PacketType::FsPutBegin:
      handleFsPutBegin(pkt);
      break;
    case PacketType::FsPutChunk:
      handleFsPutChunk(pkt);
      break;
    case PacketType::FsPutEnd:
      handleFsPutEnd(pkt);
      break;
    case PacketType::FsGetBegin:
      handleFsGetBegin(pkt);
      break;
    case PacketType::FsGetAck:
      handleFsGetAck(pkt);
      break;
    case PacketType::FsAbort:
      handleFsAbort();
      break;
    case PacketType::ErrorPacket:
      if (state_ == SessionState::ReceivingFile) host_.abortFileWrite();
      if (state_ == SessionState::FsPutting) host_.fsPutAbort();
      if (state_ == SessionState::FsGetting) host_.fsGetClose();
      state_ = SessionState::Error;
      errorMessage_ = "Cancelled by phone";
      break;
    default:
      sendError(ErrorCode::BadState, "Unexpected packet");
      break;
  }
}

void ReceiverSession::handlePairHello(const PacketView& pkt) {
  reset();
  char pin[kPinDigits]{};
  if (!host_.pairingActive(pin)) {
    sendError(ErrorCode::PairingDisabled, "Open Pair Phone on reader first");
    return;
  }
  if (pkt.payloadLength < kIdBytes + kNonceBytes + 1) {
    sendError(ErrorCode::MalformedPayload, "Bad PairHello");
    return;
  }
  memcpy(phoneId_, pkt.payload, kIdBytes);
  memcpy(clientNonce_, pkt.payload + kIdBytes, kNonceBytes);
  size_t off = kIdBytes + kNonceBytes;
  if (!readLenPrefixedString(pkt.payload, pkt.payloadLength, off, kMaxNameBytes, peerName_)) {
    sendError(ErrorCode::MalformedPayload, "Bad phone name");
    return;
  }
  if (peerName_.empty()) peerName_ = "Android Phone";

  host_.readerIdentity(readerId_, readerName_);
  host_.fillRandom(serverNonce_, kNonceBytes);
  derivePairKey(pin, clientNonce_, serverNonce_, phoneId_, readerId_, sessionKey_);

  const uint8_t rNameLen = static_cast<uint8_t>(std::min<size_t>(readerName_.size(), kMaxNameBytes));
  uint8_t resp[kIdBytes + kNonceBytes + 1 + kMaxNameBytes];
  memcpy(resp, readerId_, kIdBytes);
  memcpy(resp + kIdBytes, serverNonce_, kNonceBytes);
  resp[kIdBytes + kNonceBytes] = rNameLen;
  if (rNameLen > 0) memcpy(resp + kIdBytes + kNonceBytes + 1, readerName_.data(), rNameLen);

  if (sendPacket(PacketType::PairChallenge, resp, static_cast<uint16_t>(kIdBytes + kNonceBytes + 1 + rNameLen))) {
    state_ = SessionState::PairWaitVerify;
  }
}

void ReceiverSession::handlePairVerify(const PacketView& pkt) {
  if (state_ != SessionState::PairWaitVerify) {
    sendError(ErrorCode::BadState, "Not in pairing handshake");
    return;
  }
  char pin[kPinDigits]{};
  if (!host_.pairingActive(pin)) {
    reset();
    sendError(ErrorCode::PairingDisabled, "Pairing expired");
    return;
  }
  if (pkt.payloadLength != kSha256Bytes) {
    sendError(ErrorCode::MalformedPayload, "Bad PairVerify");
    return;
  }
  // Re-derive in case PIN was refreshed before PairHello? No: sessionKey_ was derived at PairHello;
  // verify PIN hasn't changed since PairHello.
  uint8_t currentKey[kSha256Bytes];
  derivePairKey(pin, clientNonce_, serverNonce_, phoneId_, readerId_, currentKey);
  uint8_t expectedProof[kSha256Bytes];
  computeClientPairProof(currentKey, clientNonce_, serverNonce_, expectedProof);
  if (!constantTimeEq32(expectedProof, pkt.payload)) {
    state_ = SessionState::Error;
    errorMessage_ = "Incorrect PIN";
    sendError(ErrorCode::InvalidPin, "Incorrect 6-digit code");
    return;
  }

  memcpy(sessionKey_, currentKey, kSha256Bytes);
  host_.onPairSucceeded(phoneId_, peerName_, sessionKey_);

  // Refresh settings after pairing saved.
  host_.lookupPairedPhone(phoneId_, sessionKey_, autoAccept_, defaultFolder_);

  uint8_t serverProof[kSha256Bytes];
  computeServerPairProof(sessionKey_, clientNonce_, serverNonce_, serverProof);
  if (sendPacket(PacketType::PairOk, serverProof, sizeof(serverProof))) {
    state_ = SessionState::Authenticated;
  }
}

void ReceiverSession::handleAuthHello(const PacketView& pkt) {
  reset();
  if (pkt.payloadLength < kIdBytes + kNonceBytes + 1) {
    sendError(ErrorCode::MalformedPayload, "Bad AuthHello");
    return;
  }
  memcpy(phoneId_, pkt.payload, kIdBytes);
  memcpy(clientNonce_, pkt.payload + kIdBytes, kNonceBytes);
  size_t off = kIdBytes + kNonceBytes;
  if (!readLenPrefixedString(pkt.payload, pkt.payloadLength, off, kMaxNameBytes, peerName_)) {
    sendError(ErrorCode::MalformedPayload, "Bad phone name");
    return;
  }

  if (!host_.lookupPairedPhone(phoneId_, sessionKey_, autoAccept_, defaultFolder_)) {
    sendError(ErrorCode::NotPaired, "Phone not paired with this reader");
    return;
  }
  if (peerName_.empty()) peerName_ = "Paired Phone";

  host_.readerIdentity(readerId_, readerName_);
  host_.fillRandom(serverNonce_, kNonceBytes);

  const uint8_t rNameLen = static_cast<uint8_t>(std::min<size_t>(readerName_.size(), kMaxNameBytes));
  uint8_t resp[kIdBytes + kNonceBytes + 1 + kMaxNameBytes];
  memcpy(resp, readerId_, kIdBytes);
  memcpy(resp + kIdBytes, serverNonce_, kNonceBytes);
  resp[kIdBytes + kNonceBytes] = rNameLen;
  if (rNameLen > 0) memcpy(resp + kIdBytes + kNonceBytes + 1, readerName_.data(), rNameLen);

  if (sendPacket(PacketType::AuthChallenge, resp, static_cast<uint16_t>(kIdBytes + kNonceBytes + 1 + rNameLen))) {
    state_ = SessionState::AuthWaitVerify;
  }
}

void ReceiverSession::handleAuthVerify(const PacketView& pkt) {
  if (state_ != SessionState::AuthWaitVerify) {
    sendError(ErrorCode::BadState, "Not awaiting auth");
    return;
  }
  if (pkt.payloadLength != kSha256Bytes) {
    sendError(ErrorCode::MalformedPayload, "Bad AuthVerify");
    return;
  }
  uint8_t expected[kSha256Bytes];
  computeAuthProof(sessionKey_, clientNonce_, serverNonce_, phoneId_, expected);
  if (!constantTimeEq32(expected, pkt.payload)) {
    state_ = SessionState::Error;
    errorMessage_ = "Authentication failed";
    sendError(ErrorCode::AuthFailed, "Authentication failed");
    return;
  }

  uint8_t resp[kSha256Bytes + 3];
  computeServerAuthAck(sessionKey_, clientNonce_, serverNonce_, readerId_, resp);
  resp[kSha256Bytes] = autoAccept_ ? 1 : 0;
  writeU16(resp + kSha256Bytes + 1, kDefaultChunkDataBytes);
  if (sendPacket(PacketType::AuthOk, resp, sizeof(resp))) {
    state_ = SessionState::Authenticated;
  }
}

void ReceiverSession::handleBatchOffer(const PacketView& pkt) {
  if (state_ != SessionState::Authenticated && state_ != SessionState::Completed &&
      state_ != SessionState::ReadyForFiles) {
    sendError(ErrorCode::BadState, "Authenticate before BatchOffer");
    return;
  }
  if (pkt.payloadLength < 12) {
    sendError(ErrorCode::MalformedPayload, "Bad BatchOffer");
    return;
  }
  const uint16_t count = readU16(pkt.payload);
  const uint64_t totalBytes = readU64(pkt.payload + 2);
  size_t off = 10;
  std::string firstFile;
  std::string reqFolder;
  if (count == 0 || !readLenPrefixedString(pkt.payload, pkt.payloadLength, off, kMaxFileNameBytes, firstFile) ||
      !readLenPrefixedString(pkt.payload, pkt.payloadLength, off, kMaxFolderBytes, reqFolder)) {
    sendError(ErrorCode::MalformedPayload, "Invalid BatchOffer fields");
    return;
  }

  offer_.phoneName = peerName_;
  offer_.fileCount = count;
  offer_.totalBytes = totalBytes;
  offer_.firstFileName = sanitizeBookFileName(firstFile);
  if (offer_.firstFileName.empty()) offer_.firstFileName = firstFile;
  offer_.destFolder = reqFolder.empty() ? defaultFolder_ : sanitizeDestFolder(reqFolder);

  progress_ = ActiveFileProgress{};
  progress_.fileCount = count;
  progress_.batchTotalBytes = totalBytes;
  filesSaved_ = 0;
  savedPaths_.clear();
  savedPaths_.reserve(std::min<size_t>(count, 16));

  if (autoAccept_) {
    state_ = SessionState::OfferPendingDecision;
    host_.onOfferPending(offer_);
    respondToOffer(true);
  } else {
    state_ = SessionState::OfferPendingDecision;
    host_.onOfferPending(offer_);
  }
}

void ReceiverSession::handleFileBegin(const PacketView& pkt) {
  if (state_ != SessionState::ReadyForFiles) {
    sendError(ErrorCode::BadState, "Not ready for FileBegin");
    return;
  }
  if (pkt.payloadLength < 11) {
    sendError(ErrorCode::MalformedPayload, "Bad FileBegin");
    return;
  }
  const uint16_t fileIdx = readU16(pkt.payload);
  const uint32_t fileSize = readU32(pkt.payload + 2);
  const uint32_t expectedCrc = readU32(pkt.payload + 6);
  size_t off = 10;
  std::string rawName;
  if (!readLenPrefixedString(pkt.payload, pkt.payloadLength, off, kMaxFileNameBytes, rawName)) {
    sendError(ErrorCode::MalformedPayload, "Bad FileBegin name");
    return;
  }

  const std::string cleanName = sanitizeBookFileName(rawName);
  if (cleanName.empty()) {
    uint8_t res[2] = {static_cast<uint8_t>(FileStatus::InvalidName), 0};
    sendPacket(PacketType::FileResult, res, sizeof(res));
    return;
  }

  const std::string targetFolder =
      cleanName.size() > 7 && cleanName.compare(cleanName.size() - 7, 7, ".cpfont") == 0 ? "/fonts" : offer_.destFolder;

  if (!host_.beginFileWrite(targetFolder, cleanName, fileSize)) {
    uint8_t res[2] = {static_cast<uint8_t>(FileStatus::IoError), 0};
    sendPacket(PacketType::FileResult, res, sizeof(res));
    state_ = SessionState::Error;
    errorMessage_ = "SD write failed";
    return;
  }

  progress_.fileIndex = fileIdx;
  progress_.fileName = cleanName;
  progress_.receivedBytes = 0;
  progress_.fileSize = fileSize;
  expectedFileCrc_ = expectedCrc;
  runningFileCrc_ = crc32Init();
  chunksSinceAck_ = 0;
  state_ = SessionState::ReceivingFile;

  uint8_t readyPayload[6];
  writeU16(readyPayload, fileIdx);
  writeU32(readyPayload + 2, 0);
  sendPacket(PacketType::FileReady, readyPayload, sizeof(readyPayload));
}

void ReceiverSession::handleFileChunk(const PacketView& pkt) {
  if (state_ != SessionState::ReceivingFile) {
    sendError(ErrorCode::BadState, "No open file for chunk");
    return;
  }
  if (pkt.payloadLength <= 4) {
    sendError(ErrorCode::MalformedPayload, "Empty chunk");
    return;
  }
  const uint32_t offset = readU32(pkt.payload);
  const uint8_t* bytes = pkt.payload + 4;
  const size_t dataLen = pkt.payloadLength - 4;
  if (offset != progress_.receivedBytes || progress_.receivedBytes + dataLen > progress_.fileSize) {
    host_.abortFileWrite();
    state_ = SessionState::Error;
    errorMessage_ = "Out-of-order file chunk";
    sendError(ErrorCode::MalformedPayload, "Chunk offset mismatch");
    return;
  }

  if (!host_.writeFileChunk(bytes, dataLen)) {
    host_.abortFileWrite();
    uint8_t res[2] = {static_cast<uint8_t>(FileStatus::IoError), 0};
    sendPacket(PacketType::FileResult, res, sizeof(res));
    state_ = SessionState::Error;
    errorMessage_ = "SD card write failed";
    return;
  }

  runningFileCrc_ = crc32Update(runningFileCrc_, bytes, dataLen);
  progress_.receivedBytes += static_cast<uint32_t>(dataLen);
  progress_.batchReceivedBytes += dataLen;
  ++chunksSinceAck_;

  if (chunksSinceAck_ >= kAckEveryChunks || progress_.receivedBytes == progress_.fileSize) {
    chunksSinceAck_ = 0;
    uint8_t prog[4];
    writeU32(prog, progress_.receivedBytes);
    sendPacket(PacketType::FileProgress, prog, sizeof(prog));
  }
}

void ReceiverSession::handleFileEnd(const PacketView& pkt) {
  if (state_ != SessionState::ReceivingFile || pkt.payloadLength != 4) {
    sendError(ErrorCode::BadState, "Bad FileEnd");
    return;
  }
  const uint32_t endCrc = readU32(pkt.payload);
  const uint32_t actualCrc = crc32Final(runningFileCrc_);
  if (progress_.receivedBytes != progress_.fileSize || actualCrc != expectedFileCrc_ || actualCrc != endCrc) {
    host_.abortFileWrite();
    uint8_t res[2] = {static_cast<uint8_t>(FileStatus::CrcMismatch), 0};
    sendPacket(PacketType::FileResult, res, sizeof(res));
    state_ = SessionState::ReadyForFiles;
    return;
  }

  std::string savedPath;
  if (!host_.finalizeFileWrite(savedPath)) {
    host_.abortFileWrite();
    uint8_t res[2] = {static_cast<uint8_t>(FileStatus::IoError), 0};
    sendPacket(PacketType::FileResult, res, sizeof(res));
    state_ = SessionState::Error;
    errorMessage_ = "Failed to finalize file";
    return;
  }

  ++filesSaved_;
  if (savedPaths_.size() < 16) {
    savedPaths_.push_back(savedPath);
  }
  state_ = SessionState::ReadyForFiles;

  const uint8_t pathLen = static_cast<uint8_t>(std::min<size_t>(savedPath.size(), 200));
  uint8_t res[2 + 200];
  res[0] = static_cast<uint8_t>(FileStatus::Ok);
  res[1] = pathLen;
  if (pathLen > 0) memcpy(res + 2, savedPath.data(), pathLen);
  sendPacket(PacketType::FileResult, res, static_cast<uint16_t>(2 + pathLen));
}

void ReceiverSession::handleBatchDone(const PacketView& pkt) {
  if (state_ != SessionState::ReadyForFiles || pkt.payloadLength < 2) {
    sendError(ErrorCode::BadState, "Bad BatchDone");
    return;
  }
  state_ = SessionState::Completed;
  uint8_t resp[2];
  writeU16(resp, filesSaved_);
  sendPacket(PacketType::BatchComplete, resp, sizeof(resp));
}

void ReceiverSession::sendFsOpResp(const FsStatus status, const char* msg) {
  const size_t msgLen = msg ? std::min<size_t>(strlen(msg), 160) : 0;
  uint8_t buf[3 + 160];
  buf[0] = static_cast<uint8_t>(status);
  writeU16(buf + 1, static_cast<uint16_t>(msgLen));
  if (msgLen > 0) memcpy(buf + 3, msg, msgLen);
  sendPacket(PacketType::FsOpResp, buf, static_cast<uint16_t>(3 + msgLen));
}

void ReceiverSession::handleFsStat(const PacketView& pkt) {
  if (state_ != SessionState::Authenticated) {
    sendError(ErrorCode::BadState, "Not authenticated");
    return;
  }
  size_t off = 0;
  std::string rawPath;
  if (!readU16LenString(pkt.payload, pkt.payloadLength, off, kMaxFsPathBytes, rawPath)) {
    sendError(ErrorCode::MalformedPayload, "Bad FsStatReq");
    return;
  }
  const std::string clean = sanitizeFsPath(rawPath);
  FsStatEntry entry{};
  const FsStatus st = clean.empty() ? FsStatus::Forbidden : host_.fsStat(clean, entry);

  uint8_t resp[10]{};
  resp[0] = static_cast<uint8_t>(st);
  resp[1] = (entry.exists ? 0x01u : 0u) | (entry.isDir ? 0x02u : 0u);
  writeU64(resp + 2, entry.sizeBytes);
  sendPacket(PacketType::FsStatResp, resp, sizeof(resp));
}

void ReceiverSession::handleFsList(const PacketView& pkt) {
  if (state_ != SessionState::Authenticated) {
    sendError(ErrorCode::BadState, "Not authenticated");
    return;
  }
  size_t off = 0;
  std::string rawPath;
  if (!readU16LenString(pkt.payload, pkt.payloadLength, off, kMaxFsPathBytes, rawPath)) {
    sendError(ErrorCode::MalformedPayload, "Bad FsListReq");
    return;
  }
  const std::string clean = sanitizeFsPath(rawPath);
  if (clean.empty()) {
    uint8_t endBuf[3];
    endBuf[0] = static_cast<uint8_t>(FsStatus::Forbidden);
    writeU16(endBuf + 1, 0);
    sendPacket(PacketType::FsListEnd, endBuf, sizeof(endBuf));
    return;
  }

  uint8_t chunkBuf[kDefaultChunkDataBytes];
  size_t chunkUsed = 0;
  uint16_t totalCount = 0;

  const FsStatus st = host_.fsList(clean, [&](const FsDirItem& item) -> bool {
    if (item.name.empty() || item.name.size() > 200) return true;
    const size_t entryBytes = 1 + 8 + 2 + item.name.size();
    if (chunkUsed > 0 && chunkUsed + entryBytes > sizeof(chunkBuf)) {
      if (!sendPacket(PacketType::FsListChunk, chunkBuf, static_cast<uint16_t>(chunkUsed))) {
        return false;
      }
      chunkUsed = 0;
    }
    if (entryBytes > sizeof(chunkBuf)) return true;
    chunkBuf[chunkUsed] = item.isDir ? 1 : 0;
    writeU64(chunkBuf + chunkUsed + 1, item.sizeBytes);
    writeU16(chunkBuf + chunkUsed + 9, static_cast<uint16_t>(item.name.size()));
    memcpy(chunkBuf + chunkUsed + 11, item.name.data(), item.name.size());
    chunkUsed += entryBytes;
    ++totalCount;
    return true;
  });

  if (st == FsStatus::Ok && chunkUsed > 0) {
    sendPacket(PacketType::FsListChunk, chunkBuf, static_cast<uint16_t>(chunkUsed));
  }
  uint8_t endBuf[3];
  endBuf[0] = static_cast<uint8_t>(st);
  writeU16(endBuf + 1, st == FsStatus::Ok ? totalCount : 0);
  sendPacket(PacketType::FsListEnd, endBuf, sizeof(endBuf));
}

void ReceiverSession::handleFsMkdir(const PacketView& pkt) {
  if (state_ != SessionState::Authenticated) {
    sendError(ErrorCode::BadState, "Not authenticated");
    return;
  }
  size_t off = 0;
  std::string rawPath;
  if (!readU16LenString(pkt.payload, pkt.payloadLength, off, kMaxFsPathBytes, rawPath)) {
    sendError(ErrorCode::MalformedPayload, "Bad FsMkdirReq");
    return;
  }
  const std::string clean = sanitizeFsPath(rawPath);
  if (clean.empty() || clean == "/") {
    sendFsOpResp(FsStatus::Forbidden, "Invalid directory path");
    return;
  }
  sendFsOpResp(host_.fsMkdir(clean));
}

void ReceiverSession::handleFsDelete(const PacketView& pkt) {
  if (state_ != SessionState::Authenticated) {
    sendError(ErrorCode::BadState, "Not authenticated");
    return;
  }
  if (pkt.payloadLength < 3) {
    sendError(ErrorCode::MalformedPayload, "Bad FsDeleteReq");
    return;
  }
  const uint8_t mode = pkt.payload[0];
  size_t off = 1;
  std::string rawPath;
  if (!readU16LenString(pkt.payload, pkt.payloadLength, off, kMaxFsPathBytes, rawPath)) {
    sendError(ErrorCode::MalformedPayload, "Bad FsDeleteReq");
    return;
  }
  const std::string clean = sanitizeFsPath(rawPath);
  if (clean.empty() || clean == "/") {
    sendFsOpResp(FsStatus::Forbidden, "Cannot delete root");
    return;
  }
  sendFsOpResp(host_.fsDelete(clean, mode));
}

void ReceiverSession::handleFsRename(const PacketView& pkt) {
  if (state_ != SessionState::Authenticated) {
    sendError(ErrorCode::BadState, "Not authenticated");
    return;
  }
  if (pkt.payloadLength < 5) {
    sendError(ErrorCode::MalformedPayload, "Bad FsRenameReq");
    return;
  }
  const bool overwrite = pkt.payload[0] != 0;
  size_t off = 1;
  std::string rawSrc;
  std::string rawDst;
  if (!readU16LenString(pkt.payload, pkt.payloadLength, off, kMaxFsPathBytes, rawSrc) ||
      !readU16LenString(pkt.payload, pkt.payloadLength, off, kMaxFsPathBytes, rawDst)) {
    sendError(ErrorCode::MalformedPayload, "Bad FsRenameReq");
    return;
  }
  const std::string src = sanitizeFsPath(rawSrc);
  const std::string dst = sanitizeFsPath(rawDst);
  if (src.empty() || dst.empty() || src == "/" || dst == "/") {
    sendFsOpResp(FsStatus::Forbidden, "Invalid rename path");
    return;
  }
  sendFsOpResp(host_.fsRename(src, dst, overwrite));
}

void ReceiverSession::handleFsPutBegin(const PacketView& pkt) {
  if (state_ != SessionState::Authenticated) {
    sendError(ErrorCode::BadState, "Not authenticated");
    return;
  }
  size_t off = 0;
  std::string rawPath;
  if (!readU16LenString(pkt.payload, pkt.payloadLength, off, kMaxFsPathBytes, rawPath)) {
    sendError(ErrorCode::MalformedPayload, "Bad FsPutBegin");
    return;
  }
  const std::string clean = sanitizeFsPath(rawPath);
  if (clean.empty() || clean == "/") {
    sendFsOpResp(FsStatus::Forbidden, "Invalid destination path");
    return;
  }
  const FsStatus st = host_.fsPutBegin(clean);
  if (st != FsStatus::Ok) {
    sendFsOpResp(st);
    return;
  }
  fsStreamOffset_ = 0;
  runningFileCrc_ = crc32Init();
  chunksSinceAck_ = 0;
  state_ = SessionState::FsPutting;
  sendFsOpResp(FsStatus::Ok);
}

void ReceiverSession::handleFsPutChunk(const PacketView& pkt) {
  if (state_ != SessionState::FsPutting) {
    sendError(ErrorCode::BadState, "Not in FsPut");
    return;
  }
  if (pkt.payloadLength <= 4) {
    host_.fsPutAbort();
    state_ = SessionState::Authenticated;
    sendFsOpResp(FsStatus::IoError, "Empty chunk");
    return;
  }
  const uint32_t offset = readU32(pkt.payload);
  const uint8_t* bytes = pkt.payload + 4;
  const size_t dataLen = pkt.payloadLength - 4;
  if (offset != fsStreamOffset_) {
    host_.fsPutAbort();
    state_ = SessionState::Authenticated;
    sendFsOpResp(FsStatus::IoError, "Offset mismatch");
    return;
  }
  if (!host_.fsPutChunk(bytes, dataLen)) {
    host_.fsPutAbort();
    state_ = SessionState::Authenticated;
    sendFsOpResp(FsStatus::IoError, "SD write failed");
    return;
  }
  runningFileCrc_ = crc32Update(runningFileCrc_, bytes, dataLen);
  fsStreamOffset_ += static_cast<uint32_t>(dataLen);
  ++chunksSinceAck_;
  if (chunksSinceAck_ >= kAckEveryChunks) {
    chunksSinceAck_ = 0;
    uint8_t ack[4];
    writeU32(ack, fsStreamOffset_);
    sendPacket(PacketType::FsPutAck, ack, sizeof(ack));
  }
}

void ReceiverSession::handleFsPutEnd(const PacketView& pkt) {
  if (state_ != SessionState::FsPutting) {
    sendError(ErrorCode::BadState, "Not in FsPut");
    return;
  }
  if (pkt.payloadLength != 8) {
    host_.fsPutAbort();
    state_ = SessionState::Authenticated;
    sendError(ErrorCode::MalformedPayload, "Bad FsPutEnd");
    return;
  }
  const uint32_t totalBytes = readU32(pkt.payload);
  const uint32_t expectedCrc = readU32(pkt.payload + 4);
  const uint32_t actualCrc = crc32Final(runningFileCrc_);
  if (totalBytes != fsStreamOffset_ || actualCrc != expectedCrc) {
    host_.fsPutAbort();
    state_ = SessionState::Authenticated;
    sendFsOpResp(FsStatus::CrcMismatch, "CRC mismatch");
    return;
  }
  const FsStatus st = host_.fsPutFinish();
  state_ = SessionState::Authenticated;
  sendFsOpResp(st);
}

void ReceiverSession::handleFsGetBegin(const PacketView& pkt) {
  if (state_ != SessionState::Authenticated) {
    sendError(ErrorCode::BadState, "Not authenticated");
    return;
  }
  if (pkt.payloadLength < 6) {
    sendError(ErrorCode::MalformedPayload, "Bad FsGetBegin");
    return;
  }
  const uint32_t startOffset = readU32(pkt.payload);
  size_t off = 4;
  std::string rawPath;
  if (!readU16LenString(pkt.payload, pkt.payloadLength, off, kMaxFsPathBytes, rawPath)) {
    sendError(ErrorCode::MalformedPayload, "Bad FsGetBegin");
    return;
  }
  const std::string clean = sanitizeFsPath(rawPath);
  if (clean.empty() || clean == "/") {
    uint8_t resp[10]{};
    resp[0] = static_cast<uint8_t>(FsStatus::Forbidden);
    sendPacket(PacketType::FsStatResp, resp, sizeof(resp));
    return;
  }

  uint64_t remaining = 0;
  const FsStatus st = host_.fsGetBegin(clean, startOffset, remaining);
  uint8_t resp[10]{};
  resp[0] = static_cast<uint8_t>(st);
  resp[1] = (st == FsStatus::Ok) ? 0x01u : 0u;
  writeU64(resp + 2, remaining);
  if (!sendPacket(PacketType::FsStatResp, resp, sizeof(resp)) || st != FsStatus::Ok) {
    if (st == FsStatus::Ok) host_.fsGetClose();
    return;
  }

  fsStreamOffset_ = startOffset;
  fsGetSentBytes_ = 0;
  fsGetRemainingBytes_ = remaining;
  runningFileCrc_ = crc32Init();
  chunksSinceAck_ = 0;
  state_ = SessionState::FsGetting;
  pumpFsGetWindow();
}

void ReceiverSession::pumpFsGetWindow() {
  uint8_t pktBuf[4 + kDefaultChunkDataBytes];
  while (state_ == SessionState::FsGetting && chunksSinceAck_ < kAckEveryChunks) {
    if (fsGetSentBytes_ >= fsGetRemainingBytes_) {
      host_.fsGetClose();
      state_ = SessionState::Authenticated;
      uint8_t endBuf[8];
      writeU32(endBuf, fsGetSentBytes_);
      writeU32(endBuf + 4, crc32Final(runningFileCrc_));
      sendPacket(PacketType::FsGetEnd, endBuf, sizeof(endBuf));
      return;
    }

    const size_t want = static_cast<size_t>(
        std::min<uint64_t>(kDefaultChunkDataBytes, fsGetRemainingBytes_ - fsGetSentBytes_));
    const int got = host_.fsGetRead(pktBuf + 4, want);
    if (got < 0) {
      host_.fsGetClose();
      state_ = SessionState::Authenticated;
      sendFsOpResp(FsStatus::IoError, "SD read failed");
      return;
    }
    if (got == 0) {
      host_.fsGetClose();
      state_ = SessionState::Authenticated;
      uint8_t endBuf[8];
      writeU32(endBuf, fsGetSentBytes_);
      writeU32(endBuf + 4, crc32Final(runningFileCrc_));
      sendPacket(PacketType::FsGetEnd, endBuf, sizeof(endBuf));
      return;
    }

    writeU32(pktBuf, fsStreamOffset_);
    runningFileCrc_ = crc32Update(runningFileCrc_, pktBuf + 4, static_cast<size_t>(got));
    fsStreamOffset_ += static_cast<uint32_t>(got);
    fsGetSentBytes_ += static_cast<uint32_t>(got);
    ++chunksSinceAck_;
    if (!sendPacket(PacketType::FsGetChunk, pktBuf, static_cast<uint16_t>(4 + got))) {
      host_.fsGetClose();
      state_ = SessionState::Error;
      errorMessage_ = "TX failed during FsGet";
      return;
    }
  }

  if (state_ == SessionState::FsGetting && fsGetSentBytes_ >= fsGetRemainingBytes_) {
    host_.fsGetClose();
    state_ = SessionState::Authenticated;
    uint8_t endBuf[8];
    writeU32(endBuf, fsGetSentBytes_);
    writeU32(endBuf + 4, crc32Final(runningFileCrc_));
    sendPacket(PacketType::FsGetEnd, endBuf, sizeof(endBuf));
  }
}

void ReceiverSession::handleFsGetAck(const PacketView& pkt) {
  if (state_ != SessionState::FsGetting) {
    return;
  }
  if (pkt.payloadLength != 4) {
    host_.fsGetClose();
    state_ = SessionState::Authenticated;
    sendError(ErrorCode::MalformedPayload, "Bad FsGetAck");
    return;
  }
  chunksSinceAck_ = 0;
  pumpFsGetWindow();
}

void ReceiverSession::handleFsAbort() {
  if (state_ == SessionState::FsPutting) {
    host_.fsPutAbort();
    state_ = SessionState::Authenticated;
    sendFsOpResp(FsStatus::Ok);
  } else if (state_ == SessionState::FsGetting) {
    host_.fsGetClose();
    state_ = SessionState::Authenticated;
    sendFsOpResp(FsStatus::Ok);
  } else if (state_ == SessionState::Authenticated) {
    sendFsOpResp(FsStatus::Ok);
  } else {
    sendError(ErrorCode::BadState, "Cannot abort in current state");
  }
}

bool ReceiverSession::sendPacket(const PacketType type, const void* payload, const uint16_t payloadLen) {
  size_t frameLen = 0;
  if (!encodeFrame(txFrame_, sizeof(txFrame_), type, ++txSeq_, payload, payloadLen, frameLen)) {
    return false;
  }
  return host_.sendRawBytes(txFrame_, frameLen);
}

void ReceiverSession::sendError(const ErrorCode code, const char* msg) {
  const size_t len = msg ? std::min<size_t>(strlen(msg), 120) : 0;
  uint8_t payload[2 + 120];
  payload[0] = static_cast<uint8_t>(code);
  payload[1] = static_cast<uint8_t>(len);
  if (len > 0) memcpy(payload + 2, msg, len);
  sendPacket(PacketType::ErrorPacket, payload, static_cast<uint16_t>(2 + len));
}

}  // namespace xpbt
