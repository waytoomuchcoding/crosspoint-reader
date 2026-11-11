#include "LoanBadgeUtils.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <I18n.h>
#include <TrustedTime.h>

#include <cstdio>
#include <ctime>

#include "util/Iso8601.h"

namespace loanbadge {

namespace {

constexpr int64_t kSecondsPerDay = 86400;
constexpr int64_t kPlausibleEpochFloor = 1700000000LL;

int64_t currentEpochNow() {
  const int64_t trusted = trustedtime::trustedNow();
  if (trusted > 0) return trusted;
  const time_t raw = time(nullptr);
  return (static_cast<int64_t>(raw) >= kPlausibleEpochFloor) ? static_cast<int64_t>(raw) : 0;
}

bool readLoanIdFromSidecar(const std::string& bookPath, std::string& loanIdOut) {
  loanIdOut.clear();
  const std::string sidecarPath = bookPath + ".meta.json";
  if (!Storage.exists(sidecarPath.c_str())) return false;
  std::string json;
  if (!Storage.readFileToString("LBY", sidecarPath, 2048, json)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
  const char* id = doc["libby_loan_id"] | "";
  if (id[0] == '\0') return false;
  loanIdOut = id;
  return true;
}

}  // namespace

LoanBadgeInfo computeFromTimestamps(const int64_t expiresAtEpoch, const int64_t nowEpoch) {
  LoanBadgeInfo info;
  info.isLoan = true;

  if (expiresAtEpoch <= 0 || nowEpoch <= 0) {
    info.label = tr(STR_LOAN_BADGE_BORROWED);
    return info;
  }

  const int64_t diffSec = expiresAtEpoch - nowEpoch;
  if (diffSec <= 0) {
    info.expired = true;
    info.urgent = true;
    info.daysRemaining = 0;
    info.label = tr(STR_LOAN_BADGE_EXPIRED);
    return info;
  }

  // Round up partial days (> 24h -> 2d, <= 24h -> Due Today)
  const int days = static_cast<int>((diffSec + kSecondsPerDay - 1) / kSecondsPerDay);
  info.daysRemaining = (diffSec < kSecondsPerDay) ? 0 : days;

  char buf[48];
  if (info.daysRemaining == 0) {
    info.urgent = true;
    info.label = tr(STR_LOAN_BADGE_DUE_TODAY);
  } else if (info.daysRemaining <= 3) {
    info.urgent = true;
    snprintf(buf, sizeof(buf), tr(STR_LOAN_BADGE_URGENT_DAYS), info.daysRemaining);
    info.label = buf;
  } else {
    snprintf(buf, sizeof(buf), tr(STR_LOAN_BADGE_DAYS), info.daysRemaining);
    info.label = buf;
  }
  return info;
}

LoanBadgeInfo resolveForBook(const std::string& bookPath, const std::vector<libby::Loan>& cachedLoans) {
  if (cachedLoans.empty() || bookPath.empty()) return {};

  std::string sidecarLoanId;
  const bool hasSidecarId = readLoanIdFromSidecar(bookPath, sidecarLoanId);
  const bool inLibbyDir = bookPath.rfind("/Libby/", 0) == 0;
  if (!hasSidecarId && !inLibbyDir) return {};

  const libby::Loan* matched = nullptr;
  for (const auto& loan : cachedLoans) {
    if (hasSidecarId && loan.id == sidecarLoanId) {
      matched = &loan;
      break;
    }
    if (!matched && inLibbyDir && libby::loanDestEpubPath(loan) == bookPath) {
      matched = &loan;
    }
  }

  if (!matched) return {};

  const int64_t expiresAt = iso8601::parseUtc(matched->expireDate);
  return computeFromTimestamps(expiresAt, currentEpochNow());
}

}  // namespace loanbadge
