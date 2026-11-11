#pragma once

enum class StrId {
  STR_LOAN_BADGE_BORROWED = 0,
  STR_LOAN_BADGE_EXPIRED,
  STR_LOAN_BADGE_DUE_TODAY,
  STR_LOAN_BADGE_URGENT_DAYS,
  STR_LOAN_BADGE_DAYS,
};

inline const char* stubTr(const StrId id) {
  switch (id) {
    case StrId::STR_LOAN_BADGE_BORROWED:
      return "Loan";
    case StrId::STR_LOAN_BADGE_EXPIRED:
      return "Expired";
    case StrId::STR_LOAN_BADGE_DUE_TODAY:
      return "! Due Today";
    case StrId::STR_LOAN_BADGE_URGENT_DAYS:
      return "! Due %dd";
    case StrId::STR_LOAN_BADGE_DAYS:
      return "Due %dd";
  }
  return "";
}

#define tr(id) stubTr(StrId::id)
