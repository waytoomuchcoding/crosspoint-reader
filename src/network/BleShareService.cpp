#include "BleShareService.h"

#include <Arduino.h>
#include <BlePageTurner.h>
#include <BoardConfig.h>
#include <FontCacheManager.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <esp_random.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "BleShareStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"

#if defined(SIMULATOR)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#elif defined(FREEINK_CAP_BLE_HID_HOST) && FREEINK_CAP_BLE_HID_HOST
#include <NimBLEDevice.h>
#endif

namespace bleshare {
namespace {

constexpr size_t kMinStartFreeHeapBytes = 81920;  // 49 KiB NimBLE + 32 KiB reader inflate reserve
constexpr size_t kMinStartLargestBlockBytes = 32768;
constexpr uint32_t kBackgroundAdvertiseWindowMs = 45000;
constexpr size_t kRxRingCapacity = 4096;

FontCacheManager* g_fontCache = nullptr;
bool g_pairingActive = false;
char g_pairingPin[xpbt::kPinDigits + 1] = "000000";
bool g_launchIncomingPending = false;
std::string g_launchReturnBookPath;
unsigned long g_windowArmedAtMs = 0;
bool g_radioRunning = false;

// Active streaming file state (main loop only).
HalFile g_outFile;
bool g_outFileOpen = false;
std::string g_tempFilePath;
std::string g_finalFilePath;

#if defined(SIMULATOR)
int g_listenFd = -1;
int g_clientFd = -1;

int simPort() {
  const char* env = std::getenv("CROSSPOINT_SIM_BLE_PORT");
  if (!env || env[0] == '\0') return 19843;
  return std::atoi(env);
}

void closeClientSocket() {
  if (g_clientFd >= 0) {
    ::close(g_clientFd);
    g_clientFd = -1;
  }
}

bool startTransport(bool /*fastAdv*/) {
  if (g_radioRunning) return true;
  const int port = simPort();
  if (port <= 0) return false;

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;

  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || ::listen(fd, 2) < 0) {
    LOG_ERR("BLESHARE", "Simulator TCP bridge failed to bind 127.0.0.1:%d (errno=%d)", port, errno);
    ::close(fd);
    return false;
  }
  g_listenFd = fd;
  g_radioRunning = true;
  LOG_INF("BLESHARE", "Simulator BLE bridge listening on 127.0.0.1:%d", port);
  return true;
}

void stopTransport() {
  closeClientSocket();
  if (g_listenFd >= 0) {
    ::close(g_listenFd);
    g_listenFd = -1;
  }
  g_radioRunning = false;
}

bool transportConnected() { return g_clientFd >= 0; }

bool transportSend(const uint8_t* data, size_t len) {
  if (g_clientFd < 0 || !data) return false;
  size_t sent = 0;
  while (sent < len) {
    const ssize_t n = ::send(g_clientFd, data + sent, len - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      delay(1);
      continue;
    }
    return false;
  }
  return true;
}

#elif defined(FREEINK_CAP_BLE_HID_HOST) && FREEINK_CAP_BLE_HID_HOST

portMUX_TYPE g_rxMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t g_rxRing[kRxRingCapacity]{};
size_t g_rxHead = 0;
size_t g_rxTail = 0;
size_t g_rxCount = 0;
std::atomic<bool> g_peerConnected{false};
std::atomic<bool> g_peerDisconnectEdge{false};
std::atomic<uint16_t> g_peerMtu{23};

NimBLEServer* g_server = nullptr;
NimBLECharacteristic* g_txChar = nullptr;

void pushRxBytesFromIsrOrTask(const uint8_t* data, const size_t len) {
  if (!data || len == 0) return;
  portENTER_CRITICAL(&g_rxMux);
  for (size_t i = 0; i < len; ++i) {
    if (g_rxCount >= kRxRingCapacity) break;
    g_rxRing[g_rxHead] = data[i];
    g_rxHead = (g_rxHead + 1) % kRxRingCapacity;
    ++g_rxCount;
  }
  portEXIT_CRITICAL(&g_rxMux);
}

size_t popRxBytes(uint8_t* out, const size_t maxLen) {
  portENTER_CRITICAL(&g_rxMux);
  const size_t n = std::min(maxLen, g_rxCount);
  for (size_t i = 0; i < n; ++i) {
    out[i] = g_rxRing[g_rxTail];
    g_rxTail = (g_rxTail + 1) % kRxRingCapacity;
  }
  g_rxCount -= n;
  portEXIT_CRITICAL(&g_rxMux);
  return n;
}

class ShareServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& connInfo) override {
    g_peerMtu.store(connInfo.getMTU(), std::memory_order_release);
    g_peerConnected.store(true, std::memory_order_release);
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    g_peerConnected.store(false, std::memory_order_release);
    g_peerDisconnectEdge.store(true, std::memory_order_release);
    if (g_radioRunning && NimBLEDevice::isInitialized()) {
      NimBLEDevice::startAdvertising();
    }
  }

  void onMTUChange(const uint16_t mtu, NimBLEConnInfo&) override {
    g_peerMtu.store(mtu, std::memory_order_release);
  }
};

class ShareRxCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
    if (!chr) return;
    const NimBLEAttValue val = chr->getValue();
    if (val.length() > 0) {
      pushRxBytesFromIsrOrTask(val.data(), val.length());
    }
  }
};

ShareServerCallbacks g_serverCb;
ShareRxCallbacks g_rxCb;

bool heapAllowsStart() {
#ifdef BOARD_HAS_PSRAM
  if (HalMemory::getPsramHeap().totalBytes > 0) return true;
#endif
  if (g_fontCache) {
    RenderLock lock(RenderLock::Mode::Try);
    if (lock.ownsLock()) {
      g_fontCache->releaseSdFontCaches();
    }
  }
  const auto h = HalMemory::getInternalHeap();
  return h.freeBytes >= kMinStartFreeHeapBytes && h.largestBlockBytes >= kMinStartLargestBlockBytes;
}

bool startTransport(const bool fastAdv) {
  if (g_radioRunning) return true;
  if (bleturner::holdsHeap() || bleturner::status().running || bleturner::status().starting ||
      bleturner::status().stopping) {
    return false;
  }
  if (!heapAllowsStart()) {
    LOG_DBG("BLESHARE", "Deferring BLE share advertise: insufficient contiguous internal heap");
    return false;
  }

  if (NimBLEDevice::isInitialized()) {
    NimBLEDevice::deinit(true);
    delay(20);
  }
  if (!NimBLEDevice::init(deviceDisplayName())) {
    LOG_ERR("BLESHARE", "NimBLEDevice::init failed");
    return false;
  }

  NimBLEDevice::setMTU(255);
  portENTER_CRITICAL(&g_rxMux);
  g_rxHead = g_rxTail = g_rxCount = 0;
  portEXIT_CRITICAL(&g_rxMux);
  g_peerConnected.store(false, std::memory_order_release);
  g_peerDisconnectEdge.store(false, std::memory_order_release);
  g_peerMtu.store(23, std::memory_order_release);

  g_server = NimBLEDevice::createServer();
  if (!g_server) {
    NimBLEDevice::deinit(true);
    return false;
  }
  g_server->setCallbacks(&g_serverCb);
  g_server->advertiseOnDisconnect(true);

  NimBLEService* svc = g_server->createService(xpbt::kServiceUuid);
  if (!svc) {
    g_server = nullptr;
    NimBLEDevice::deinit(true);
    return false;
  }

  NimBLECharacteristic* rxChar =
      svc->createCharacteristic(xpbt::kRxCharUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  g_txChar = svc->createCharacteristic(xpbt::kTxCharUuid, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ);
  if (!rxChar || !g_txChar) {
    g_txChar = nullptr;
    g_server = nullptr;
    NimBLEDevice::deinit(true);
    return false;
  }
  rxChar->setCallbacks(&g_rxCb);
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->reset();
  adv->addServiceUUID(xpbt::kServiceUuid);
  adv->enableScanResponse(true);
  adv->setName(deviceDisplayName());
  adv->setMinInterval(fastAdv ? 0x0060 : 0x0190);  // 60ms in foreground/pairing, 250ms background
  adv->setMaxInterval(fastAdv ? 0x00C0 : 0x0320);  // 120ms in foreground/pairing, 500ms background
  if (!adv->start()) {
    LOG_ERR("BLESHARE", "Advertising start failed");
    g_txChar = nullptr;
    g_server = nullptr;
    NimBLEDevice::deinit(true);
    return false;
  }

  g_radioRunning = true;
  LOG_INF("BLESHARE", "BLE companion share advertising (%s)", fastAdv ? "fast" : "background");
  return true;
}

void stopTransport() {
  if (!g_radioRunning) return;
  g_txChar = nullptr;
  g_server = nullptr;
  g_peerConnected.store(false, std::memory_order_release);
  g_peerDisconnectEdge.store(false, std::memory_order_release);
  if (NimBLEDevice::isInitialized()) {
    NimBLEDevice::deinit(true);
  }
  portENTER_CRITICAL(&g_rxMux);
  g_rxHead = g_rxTail = g_rxCount = 0;
  portEXIT_CRITICAL(&g_rxMux);
  g_radioRunning = false;
  LOG_INF("BLESHARE", "BLE companion share stopped");
}

bool transportConnected() { return g_peerConnected.load(std::memory_order_acquire); }

bool transportSend(const uint8_t* data, const size_t len) {
  if (!g_radioRunning || !g_txChar || !transportConnected() || !data) return false;
  const uint16_t mtu = g_peerMtu.load(std::memory_order_acquire);
  const size_t maxSlice = mtu > 6 ? static_cast<size_t>(mtu - 3) : 20;
  size_t off = 0;
  while (off < len) {
    const size_t slice = std::min(len - off, maxSlice);
    if (!g_txChar->notify(data + off, slice)) {
      return false;
    }
    off += slice;
    if (off < len) delay(2);
  }
  return true;
}

#else
bool startTransport(bool) { return false; }
void stopTransport() {}
bool transportConnected() { return false; }
bool transportSend(const uint8_t*, size_t) { return false; }
#endif

class FirmwareSessionHost final : public xpbt::SessionHost {
 public:
  void readerIdentity(uint8_t outReaderId[xpbt::kIdBytes], std::string& outReaderName) override {
    if (!BLESHARE_STORE.getReaderIdBytes(outReaderId)) {
      uint8_t rnd[xpbt::kIdBytes];
      esp_fill_random(rnd, sizeof(rnd));
      BLESHARE_STORE.ensureReaderId(rnd);
      BLESHARE_STORE.getReaderIdBytes(outReaderId);
    }
    outReaderName = deviceDisplayName();
  }

  void fillRandom(uint8_t* out, const size_t len) override { esp_fill_random(out, len); }

  bool pairingActive(char outPin[xpbt::kPinDigits]) override {
    if (!g_pairingActive) return false;
    memcpy(outPin, g_pairingPin, xpbt::kPinDigits);
    return true;
  }

  void onPairSucceeded(const uint8_t phoneId[xpbt::kIdBytes], const std::string& phoneName,
                       const uint8_t sharedKey[xpbt::kSha256Bytes]) override {
    BLESHARE_STORE.setPairedPhone(phoneId, phoneName, sharedKey);
    g_pairingActive = false;
    LOG_INF("BLESHARE", "Paired phone '%s'", BLESHARE_STORE.getPairedPhoneName().c_str());
  }

  bool lookupPairedPhone(const uint8_t phoneId[xpbt::kIdBytes], uint8_t outSharedKey[xpbt::kSha256Bytes],
                         bool& outAutoAccept, std::string& outDefaultFolder) override {
    if (!BLESHARE_STORE.matchPairedPhone(phoneId, outSharedKey)) return false;
    outAutoAccept = BLESHARE_STORE.isAutoAccept();
    outDefaultFolder = BLESHARE_STORE.getDestFolder();
    return true;
  }

  void onOfferPending(const xpbt::BatchOfferInfo& offer) override {
    LOG_INF("BLESHARE", "Incoming batch offer from '%s': %u file(s), %llu bytes, first='%s'",
            offer.phoneName.c_str(), static_cast<unsigned>(offer.fileCount),
            static_cast<unsigned long long>(offer.totalBytes), offer.firstFileName.c_str());
  }

  bool beginFileWrite(const std::string& destFolder, const std::string& cleanFileName, uint32_t) override {
    abortFileWrite();
    if (!Storage.exists(destFolder.c_str())) {
      Storage.mkdir(destFolder.c_str());
    }
    g_finalFilePath = destFolder + "/" + cleanFileName;
    g_tempFilePath = destFolder + "/." + cleanFileName + ".xpbt";
    if (Storage.exists(g_tempFilePath.c_str())) {
      Storage.remove(g_tempFilePath.c_str());
    }
    if (!Storage.openFileForWrite("BLS", g_tempFilePath.c_str(), g_outFile)) {
      LOG_ERR("BLESHARE", "Failed to open temp path %s", g_tempFilePath.c_str());
      return false;
    }
    g_outFileOpen = true;
    return true;
  }

  bool writeFileChunk(const uint8_t* data, const size_t len) override {
    if (!g_outFileOpen) return false;
    return g_outFile.write(data, len) == len;
  }

  bool finalizeFileWrite(std::string& outSavedPath) override {
    if (!g_outFileOpen) return false;
    g_outFile.close();
    g_outFileOpen = false;
    if (Storage.exists(g_finalFilePath.c_str())) {
      Storage.remove(g_finalFilePath.c_str());
    }
    if (!Storage.rename(g_tempFilePath.c_str(), g_finalFilePath.c_str())) {
      Storage.remove(g_tempFilePath.c_str());
      return false;
    }
    outSavedPath = g_finalFilePath;
    if (outSavedPath.size() > 7 && outSavedPath.compare(outSavedPath.size() - 7, 7, ".cpfont") == 0) {
      sdFontSystem.markRegistryDirty();
    } else if (library::isLibraryBookFile(outSavedPath)) {
      library::markLibraryIndexDirty();
    }
    LOG_INF("BLESHARE", "Saved book: %s", outSavedPath.c_str());
    return true;
  }

  void abortFileWrite() override {
    if (g_outFileOpen) {
      g_outFile.close();
      g_outFileOpen = false;
    }
    if (!g_tempFilePath.empty() && Storage.exists(g_tempFilePath.c_str())) {
      Storage.remove(g_tempFilePath.c_str());
    }
    g_tempFilePath.clear();
  }

  bool sendRawBytes(const uint8_t* data, const size_t len) override { return transportSend(data, len); }
};

FirmwareSessionHost g_host;
xpbt::ReceiverSession g_session(g_host);

}  // namespace

void begin(FontCacheManager* fontCache) {
  g_fontCache = fontCache;
  BLESHARE_STORE.loadFromFile();
  uint8_t rnd[xpbt::kIdBytes];
  esp_fill_random(rnd, sizeof(rnd));
  BLESHARE_STORE.ensureReaderId(rnd);
  g_windowArmedAtMs = millis();
}

const char* deviceDisplayName() {
#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  return gpio.deviceIsX3() ? "CrossPoint X3" : "CrossPoint X4";
#else
  return "CrossPoint Reader";
#endif
}

bool ownsRadio() { return g_radioRunning; }

bool hasLiveLink() { return g_radioRunning && transportConnected(); }

void stopRadio() {
  g_host.abortFileWrite();
  g_session.reset();
  stopTransport();
}

bool startPairingMode() {
  uint32_t code = esp_random() % 1000000u;
#if defined(SIMULATOR)
  if (const char* fixed = std::getenv("CROSSPOINT_SIM_PAIR_PIN"); fixed && std::strlen(fixed) == xpbt::kPinDigits) {
    std::memcpy(g_pairingPin, fixed, xpbt::kPinDigits);
    g_pairingPin[xpbt::kPinDigits] = '\0';
  } else {
    std::snprintf(g_pairingPin, sizeof(g_pairingPin), "%06u", static_cast<unsigned>(code));
  }
#else
  std::snprintf(g_pairingPin, sizeof(g_pairingPin), "%06u", static_cast<unsigned>(code));
#endif
  g_pairingActive = true;
  g_windowArmedAtMs = millis();
  g_session.reset();
  if (!g_radioRunning) {
    startTransport(/*fastAdv=*/true);
  }
  LOG_INF("BLESHARE", "Pairing mode active (PIN=%s)", g_pairingPin);
  return g_radioRunning;
}

void stopPairingMode() { g_pairingActive = false; }

bool isPairingActive() { return g_pairingActive; }

const char* pairingPin() { return g_pairingPin; }

xpbt::SessionState sessionState() { return g_session.state(); }
const xpbt::BatchOfferInfo& currentOffer() { return g_session.offer(); }
const xpbt::ActiveFileProgress& currentProgress() { return g_session.progress(); }
const std::vector<std::string>& savedPaths() { return g_session.savedPaths(); }
const std::string& errorMessage() { return g_session.errorMessage(); }
const std::string& peerName() { return g_session.peerName(); }

bool respondToOffer(const bool accept) { return g_session.respondToOffer(accept); }

void cancelTransfer() {
  g_host.abortFileWrite();
  g_session.reset();
#if defined(SIMULATOR)
  closeClientSocket();
#endif
}

void clearCompletedState() {
  if (g_session.state() == xpbt::SessionState::Completed || g_session.state() == xpbt::SessionState::Deferred ||
      g_session.state() == xpbt::SessionState::Error) {
    g_session.reset();
  }
}

bool consumeIncomingLaunch(std::string& outReturnBookPath) {
  if (!g_launchIncomingPending) return false;
  g_launchIncomingPending = false;
  outReturnBookPath = std::move(g_launchReturnBookPath);
  g_launchReturnBookPath.clear();
  return true;
}

bool tick(const Scene& scene) {
  const unsigned long now = millis();
  if (scene.localActivity || scene.shareScreenForeground) {
    g_windowArmedAtMs = now;
  }

  // Tear down immediately if Wi-Fi or raw USB storage is active, or if the BLE HID page turner claims the radio.
  if (scene.storageBusy || scene.wifiOn || (!scene.shareScreenForeground && scene.pageTurnerWantsRadio)) {
    if (g_radioRunning) stopRadio();
    return false;
  }

  const bool backgroundEligible =
      BLESHARE_STORE.isEnabled() && BLESHARE_STORE.isPaired() && scene.canShowIncomingPopup &&
      (now - g_windowArmedAtMs < kBackgroundAdvertiseWindowMs);
  const bool wantsRadio = scene.shareScreenForeground || g_pairingActive || backgroundEligible;

  if (!g_radioRunning) {
    if (wantsRadio) {
      startTransport(/*fastAdv=*/scene.shareScreenForeground || g_pairingActive);
    }
    if (!g_radioRunning) return false;
  } else if (!wantsRadio && !transportConnected()) {
    // Idle background window expired with no phone connected: power down NimBLE so C3 drops to 10 MHz.
    stopRadio();
    return false;
  }

  bool hadActivity = false;

#if defined(SIMULATOR)
  if (g_clientFd < 0 && g_listenFd >= 0) {
    sockaddr_in peer{};
    socklen_t peerLen = sizeof(peer);
    const int cfd = ::accept(g_listenFd, reinterpret_cast<sockaddr*>(&peer), &peerLen);
    if (cfd >= 0) {
      const int flags = ::fcntl(cfd, F_GETFL, 0);
      ::fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
      g_clientFd = cfd;
      g_windowArmedAtMs = now;
      hadActivity = true;
      LOG_INF("BLESHARE", "Simulator client connected");
    }
  }
  if (g_clientFd >= 0) {
    uint8_t buf[240];
    for (int iter = 0; iter < 16; ++iter) {
      const ssize_t n = ::recv(g_clientFd, buf, sizeof(buf), 0);
      if (n > 0) {
        hadActivity = true;
        g_windowArmedAtMs = now;
        g_session.ingestBytes(buf, static_cast<size_t>(n));
      } else if (n == 0) {
        closeClientSocket();
        g_session.onDisconnected();
        break;
      } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          closeClientSocket();
          g_session.onDisconnected();
        }
        break;
      }
    }
  }
#elif defined(FREEINK_CAP_BLE_HID_HOST) && FREEINK_CAP_BLE_HID_HOST
  if (g_peerDisconnectEdge.exchange(false, std::memory_order_acq_rel)) {
    g_session.onDisconnected();
  }
  uint8_t buf[240];
  for (int iter = 0; iter < 16; ++iter) {
    const size_t n = popRxBytes(buf, sizeof(buf));
    if (n == 0) break;
    hadActivity = true;
    g_windowArmedAtMs = now;
    g_session.ingestBytes(buf, n);
  }
#endif

  const auto st = g_session.state();
  if (!scene.shareScreenForeground && scene.canShowIncomingPopup &&
      (st == xpbt::SessionState::OfferPendingDecision || st == xpbt::SessionState::ReceivingFile)) {
    g_launchIncomingPending = true;
    g_launchReturnBookPath = scene.inReader ? scene.currentBookPath : std::string();
    hadActivity = true;
  }

  return hadActivity;
}

}  // namespace bleshare
