#pragma once

#include <BleShareSession.h>

#include <cstdint>
#include <string>
#include <vector>

class FontCacheManager;

namespace bleshare {

struct Scene {
  bool shareScreenForeground = false;
  bool canShowIncomingPopup = false;
  bool inReader = false;
  bool pageTurnerWantsRadio = false;
  bool storageBusy = false;
  bool wifiOn = false;
  bool localActivity = false;
  std::string currentBookPath;
};

void begin(FontCacheManager* fontCache);

// Drives the transport, processes incoming packets on the main loop, and manages
// background BLE advertising and power-saving idle shutdown.
// Returns true when link or transfer activity occurred this pass.
bool tick(const Scene& scene);

// True while BleShare owns the NimBLE stack (or simulator listener).
bool ownsRadio();

// True while a phone is actively connected.
bool hasLiveLink();

// Stops the BLE peripheral stack (or simulator listener) immediately, freeing heap.
void stopRadio();

// Enters 6-digit PIN pairing mode and ensures the BLE radio is advertising.
bool startPairingMode();
void stopPairingMode();
bool isPairingActive();
const char* pairingPin();
const char* deviceDisplayName();

xpbt::SessionState sessionState();
const xpbt::BatchOfferInfo& currentOffer();
const xpbt::ActiveFileProgress& currentProgress();
const std::vector<std::string>& savedPaths();
const std::string& errorMessage();
const std::string& peerName();

bool respondToOffer(bool accept);
void cancelTransfer();
void clearCompletedState();

// Returns true once when a paired phone connects in the background and offers files
// while BleShareActivity is not yet the foreground screen.
bool consumeIncomingLaunch(std::string& outReturnBookPath);

}  // namespace bleshare
