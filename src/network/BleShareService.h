#pragma once

#include <BleShareSession.h>

#include <cstdint>
#include <string>
#include <vector>

class FontCacheManager;

namespace bleshare {

struct Scene {
  bool shareScreenForeground = false;
  bool inReader = false;
  bool storageBusy = false;
  bool wifiOn = false;
  std::string currentBookPath;
};

void begin(FontCacheManager* fontCache);
bool isSupported();

// Drives the transport and processes incoming packets while the BleShare screen is active.
// Returns true when link or transfer activity occurred this pass.
bool tick(const Scene& scene);

// True while BleShare owns the NimBLE stack (or simulator listener).
bool ownsRadio();

// True while a phone is actively connected.
bool hasLiveLink();

// Starts/stops the BLE peripheral stack (or simulator listener) immediately.
bool startRadio();
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

}  // namespace bleshare
