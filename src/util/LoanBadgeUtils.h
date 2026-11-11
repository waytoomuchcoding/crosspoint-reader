#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "network/LibbyClient.h"

namespace loanbadge {

struct LoanBadgeInfo {
  bool isLoan = false;
  bool urgent = false;
  bool expired = false;
  int daysRemaining = -1;
  std::string label;
};

// Pure timestamp-based calculation (exposed for deterministic unit tests).
// When `nowEpoch <= 0`, `isLoan` is set true with a generic borrowed badge (`Loan`).
LoanBadgeInfo computeFromTimestamps(int64_t expiresAtEpoch, int64_t nowEpoch);

// Cross-references a book path (`/Libby/<Title>.epub` or `<bookPath>.meta.json` `libby_loan_id`)
// against `cachedLoans` to produce a localized loan countdown badge.
LoanBadgeInfo resolveForBook(const std::string& bookPath, const std::vector<libby::Loan>& cachedLoans);

}  // namespace loanbadge
