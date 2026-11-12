#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace libby {

struct Loan {
  std::string id;
  std::string cardId;
  std::string title;
  std::string author;
  std::string expireDate;
  bool hasOpenEpub = false;   // "ebook-epub-open" (DRM-free EPUB)
  bool hasAdobeEpub = false;  // "ebook-epub-adobe" (Adobe ADEPT .acsm -> .epub + .epub.key)

  bool isDownloadable() const { return hasOpenEpub || hasAdobeEpub; }
};

// Token management (stored obfuscated at `/.crosspoint/libby.json`, with read fallback to SD Libby plugin)
bool hasToken();
bool loadToken(std::string& tokenOut);
bool saveToken(const std::string& token);
bool loadCachedLoans(std::vector<Loan>& loansOut);

// Parses `/chip/sync` JSON payload from memory or streamed directly from an SD file using an ArduinoJson filter.
bool parseSyncLoansJson(std::string_view syncJson, std::vector<Loan>& loansOut, std::string& errOut);
bool parseSyncLoansFile(const char* filePath, std::vector<Loan>& loansOut, std::string& errOut);

// Builds the URL-encoded OverDrive fulfillment endpoint URL for `loan`.
std::string buildFulfillEndpointUrl(const Loan& loan);

// Serializes the `.meta.json` sidecar tracking `libby_loan_id` and `libby_card_id` with proper JSON escaping.
std::string serializeLoanSidecarJson(const Loan& loan);

// Pairs with Libby using an 8-digit setup code from the Libby mobile/web app ("Copy To Another Device").
bool pairWithSetupCode(std::string_view setupCode, std::string& errOut);

// Fetches active loans from `https://sentry-read.svc.overdrive.com/chip/sync`.
bool fetchLoans(std::vector<Loan>& loansOut, std::string& errOut);

// Computes the canonical SD destination path `/Libby/<Title>.epub` for a loan.
std::string loanDestEpubPath(const Loan& loan);

// Downloads and fulfills a loan into `/Libby/<Title>.epub` (plus `/Libby/<Title>.epub.key` for Adobe DRM loans).
bool downloadLoan(const Loan& loan, std::string& destEpubPathOut, std::string& errOut,
                  std::function<void(size_t, size_t)> progressCb = nullptr);

// Syncs active loans via `/chip/sync` and automatically downloads/fulfills any downloadable EPUB loan
// not yet present on the SD card.
// shouldStop (optional): checked before each loan download; true ends the sync.
bool syncAndFulfillNewLoans(int& fulfilledCountOut, std::string& errOut,
                            const std::function<bool()>& shouldStop = nullptr);

}  // namespace libby
