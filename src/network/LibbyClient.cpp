#include "LibbyClient.h"

#include <ArduinoJson.h>
#include <CredentialIntegrity.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cctype>
#include <cstring>

#include "network/AdeptFulfiller.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/PluginHttp.h"
#include "util/StringUtils.h"

namespace libby {

namespace {

constexpr char kSentryBaseUrl[] = "https://sentry-read.svc.overdrive.com";
constexpr char kPrimaryTokenPath[] = "/.crosspoint/libby.json";
constexpr char kLegacyPluginTokenPath1[] = "/.crosspoint/plugins/libby/token.json";
constexpr char kLegacyPluginTokenPath2[] = "/plugins/libby/token.json";
constexpr char kCachedLoansPath[] = "/.crosspoint/plugins/libby/loans.json";
constexpr char kSyncTmpPath[] = "/.crosspoint/libby_sync.tmp";

struct HalFileJsonReader {
  HalFile& file;

  int read() {
    uint8_t b = 0;
    return file.read(&b, 1) == 1 ? static_cast<int>(b) : -1;
  }

  size_t readBytes(char* buffer, size_t length) {
    const int n = file.read(reinterpret_cast<uint8_t*>(buffer), length);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }
};

std::string cleanSetupCode(std::string_view raw) {
  std::string digits;
  digits.reserve(8);
  for (char c : raw) {
    if (c >= '0' && c <= '9') {
      digits.push_back(c);
    }
  }
  return digits;
}

void buildLoansFilter(JsonDocument& filter) {
  JsonObject loanFilter = filter["loans"].add<JsonObject>();
  loanFilter["id"] = true;
  loanFilter["cardId"] = true;
  loanFilter["title"] = true;
  loanFilter["firstCreatorName"] = true;
  loanFilter["expireDate"] = true;
  loanFilter["formats"][0]["id"] = true;
}

void extractLoansFromDoc(const JsonDocument& doc, std::vector<Loan>& loansOut) {
  JsonArrayConst loansArr = doc["loans"].as<JsonArrayConst>();
  if (loansArr.isNull()) {
    return;
  }

  for (JsonVariantConst item : loansArr) {
    Loan loan;
    loan.id = item["id"] | "";
    loan.cardId = item["cardId"] | "";
    loan.title = item["title"] | "";
    loan.author = item["firstCreatorName"] | "";
    loan.expireDate = item["expireDate"] | "";

    JsonArrayConst formats = item["formats"].as<JsonArrayConst>();
    if (!formats.isNull()) {
      for (JsonVariantConst fmt : formats) {
        const char* fmtId = fmt["id"] | "";
        if (strcmp(fmtId, "ebook-epub-open") == 0) {
          loan.hasOpenEpub = true;
        } else if (strcmp(fmtId, "ebook-epub-adobe") == 0) {
          loan.hasAdobeEpub = true;
        }
      }
    }

    if (!loan.id.empty() && !loan.title.empty()) {
      loansOut.push_back(std::move(loan));
    }
  }
}

}  // namespace

bool loadToken(std::string& tokenOut) {
  tokenOut.clear();
  static constexpr const char* kPaths[] = {kPrimaryTokenPath, kLegacyPluginTokenPath1, kLegacyPluginTokenPath2};
  for (const char* path : kPaths) {
    if (!Storage.exists(path)) continue;
    std::string json;
    if (!Storage.readFileToString("LBY", path, 8 * 1024, json)) continue;
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) continue;

    const char* obf = doc["token_obf"] | "";
    if (obf[0] != '\0') {
      bool ok = false;
      std::string decoded = obfuscation::deobfuscateFromBase64(obf, &ok);
      if (ok && !decoded.empty()) {
        if (doc["token_crc32"].is<uint32_t>()) {
          const uint32_t expectedCrc = doc["token_crc32"].as<uint32_t>();
          if (!credential_integrity::validate(decoded, decoded.size(), expectedCrc)) {
            continue;
          }
        }
        tokenOut = std::move(decoded);
        return true;
      }
    }

    const char* plain = doc["token"] | doc["identity"] | "";
    if (plain[0] != '\0') {
      tokenOut = plain;
      return true;
    }
  }
  return false;
}

bool hasToken() {
  std::string tok;
  return loadToken(tok);
}

bool saveToken(const std::string& token) {
  if (token.empty()) return false;
  Storage.ensureDirectoryExists("/.crosspoint");

  JsonDocument doc;
  doc["token_obf"] = obfuscation::obfuscateToBase64(token);
  doc["token_crc32"] = credential_integrity::crc32(token);

  std::string payload;
  serializeJson(doc, payload);

  const std::string tmpPath = std::string(kPrimaryTokenPath) + ".tmp";
  HalFile f;
  if (!Storage.openFileForWrite("LBY", tmpPath, f)) {
    return false;
  }
  const bool written = (f.write(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()) == payload.size());
  const bool closed = f.close();
  if (!written || !closed) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return Storage.replaceFile(tmpPath.c_str(), kPrimaryTokenPath);
}

bool loadCachedLoans(std::vector<Loan>& loansOut) {
  std::string unusedErr;
  return parseSyncLoansFile(kCachedLoansPath, loansOut, unusedErr);
}

bool parseSyncLoansJson(std::string_view syncJson, std::vector<Loan>& loansOut, std::string& errOut) {
  loansOut.clear();
  if (syncJson.empty()) {
    errOut = "Empty /chip/sync response";
    return false;
  }

  JsonDocument filter;
  buildLoansFilter(filter);

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, syncJson.data(), syncJson.size(), DeserializationOption::Filter(filter));
  if (err != DeserializationError::Ok) {
    errOut = std::string("Failed to parse Libby sync JSON: ") + err.c_str();
    return false;
  }

  extractLoansFromDoc(doc, loansOut);
  return true;
}

bool parseSyncLoansFile(const char* filePath, std::vector<Loan>& loansOut, std::string& errOut) {
  loansOut.clear();
  if (!filePath || !Storage.exists(filePath)) {
    errOut = "Missing cached loans file";
    return false;
  }

  HalFile f;
  if (!Storage.openFileForRead("LBY", filePath, f)) {
    errOut = "Failed to open synced Libby loans file";
    return false;
  }
  if (f.size() == 0) {
    f.close();
    errOut = "Empty /chip/sync response";
    return false;
  }

  JsonDocument filter;
  buildLoansFilter(filter);

  JsonDocument doc;
  HalFileJsonReader reader{f};
  const DeserializationError err = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
  f.close();
  if (err != DeserializationError::Ok) {
    errOut = std::string("Failed to parse Libby sync JSON: ") + err.c_str();
    return false;
  }

  extractLoansFromDoc(doc, loansOut);
  return true;
}

std::string buildFulfillEndpointUrl(const Loan& loan) {
  const std::string formatId = loan.hasOpenEpub ? "ebook-epub-open" : "ebook-epub-adobe";
  return std::string(kSentryBaseUrl) + "/card/" + pluginhttp::urlEncodeQuery(loan.cardId) + "/loan/" +
         pluginhttp::urlEncodeQuery(loan.id) + "/fulfill/" + pluginhttp::urlEncodeQuery(formatId);
}

std::string serializeLoanSidecarJson(const Loan& loan) {
  JsonDocument doc;
  doc["libby_loan_id"] = loan.id;
  doc["libby_card_id"] = loan.cardId;
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool pairWithSetupCode(std::string_view setupCode, std::string& errOut) {
  const std::string code = cleanSetupCode(setupCode);
  if (code.size() != 8) {
    errOut = "Setup code must be 8 digits";
    return false;
  }

  // Step 1: Acquire an anonymous chip identity
  pluginhttp::Headers chipHeaders = {
      {"Accept", "application/json"},
      {"User-Agent", "Mozilla/5.0 (CrossPoint)"},
  };
  String chipResp;
  const int chipStatus = pluginhttp::request(nullptr, std::string(kSentryBaseUrl) + "/chip?client-id=dewey", "POST", "",
                                             chipHeaders, chipResp, 16 * 1024);
  if (chipStatus < 200 || chipStatus >= 300 || chipResp.length() == 0) {
    errOut = "Failed to acquire Libby chip identity (HTTP " + std::to_string(chipStatus) + ")";
    return false;
  }

  JsonDocument chipDoc;
  if (deserializeJson(chipDoc, chipResp.c_str(), chipResp.length()) != DeserializationError::Ok) {
    errOut = "Invalid JSON from Libby /chip endpoint";
    return false;
  }
  const std::string identity = chipDoc["identity"] | "";
  chipResp = String();
  chipDoc.clear();
  if (identity.empty()) {
    errOut = "Missing identity token in Libby /chip response";
    return false;
  }

  // Step 2: Link the 8-digit setup code via POST /chip/clone/code
  pluginhttp::Headers cloneHeaders = {
      {"Authorization", "Bearer " + identity},
      {"Content-Type", "application/json; charset=UTF-8"},
      {"Accept", "application/json"},
      {"User-Agent", "Mozilla/5.0 (CrossPoint)"},
  };
  const std::string cloneBody = "{\"code\":\"" + code + "\"}";
  String cloneResp;
  const int cloneStatus = pluginhttp::request(nullptr, std::string(kSentryBaseUrl) + "/chip/clone/code", "POST",
                                              cloneBody, cloneHeaders, cloneResp, 16 * 1024);
  if (cloneStatus < 200 || cloneStatus >= 300) {
    errOut = "Libby setup code rejected (HTTP " + std::to_string(cloneStatus) + ")";
    return false;
  }

  std::string finalToken = identity;
  if (cloneResp.length() > 0) {
    JsonDocument cloneDoc;
    if (deserializeJson(cloneDoc, cloneResp.c_str(), cloneResp.length()) == DeserializationError::Ok) {
      const char* updated = cloneDoc["identity"] | cloneDoc["token"] | "";
      if (updated[0] != '\0') {
        finalToken = updated;
      }
    }
  }

  if (!saveToken(finalToken)) {
    errOut = "Failed to save Libby token to SD card";
    return false;
  }
  return true;
}

bool fetchLoans(std::vector<Loan>& loansOut, std::string& errOut) {
  loansOut.clear();
  std::string token;
  if (!loadToken(token)) {
    errOut = "Not signed in to Libby";
    return false;
  }

  Storage.ensureDirectoryExists("/.crosspoint");
  pluginhttp::Headers headers = {
      {"Authorization", "Bearer " + token},
      {"Accept", "application/json"},
      {"User-Agent", "Mozilla/5.0 (CrossPoint)"},
  };

  const int status = pluginhttp::requestToFile(nullptr, std::string(kSentryBaseUrl) + "/chip/sync", "GET", "", headers,
                                               kSyncTmpPath, 512 * 1024);
  if (status < 200 || status >= 300) {
    Storage.remove(kSyncTmpPath);
    errOut = "Failed to sync Libby loans (HTTP " + std::to_string(status) + ")";
    return false;
  }

  Storage.ensureDirectoryExists("/.crosspoint/plugins");
  Storage.ensureDirectoryExists("/.crosspoint/plugins/libby");
  if (!Storage.replaceFile(kSyncTmpPath, kCachedLoansPath)) {
    Storage.remove(kSyncTmpPath);
    errOut = "Failed to store synced Libby loans on SD";
    return false;
  }

  return parseSyncLoansFile(kCachedLoansPath, loansOut, errOut);
}

std::string loanDestEpubPath(const Loan& loan) {
  const std::string sanitized = StringUtils::sanitizeFilenamePreservingExtension(loan.title + ".epub");
  return "/Libby/" + (sanitized.empty() ? (loan.id + ".epub") : sanitized);
}

bool downloadLoan(const Loan& loan, std::string& destEpubPathOut, std::string& errOut,
                  std::function<void(size_t, size_t)> progressCb) {
  if (!loan.isDownloadable()) {
    errOut = "This loan does not offer an EPUB format (Kindle-only or audiobook)";
    return false;
  }

  std::string token;
  if (!loadToken(token)) {
    errOut = "Not signed in to Libby";
    return false;
  }

  if (!Storage.exists("/Libby")) {
    Storage.mkdir("/Libby");
  }
  destEpubPathOut = loanDestEpubPath(loan);

  const std::string fulfillEndpoint = buildFulfillEndpointUrl(loan);

  pluginhttp::Headers authHeaders = {
      {"Authorization", "Bearer " + token},
      {"Accept", "*/*"},
      {"User-Agent", "Mozilla/5.0 (CrossPoint)"},
  };

  if (loan.hasOpenEpub) {
    String resp;
    const int status = pluginhttp::request(nullptr, fulfillEndpoint, "GET", "", authHeaders, resp, 16 * 1024);
    std::string downloadUrl = fulfillEndpoint;
    bool sendBearer = true;
    if (status >= 200 && status < 300 && resp.length() > 0 && resp.c_str()[0] == '{') {
      JsonDocument doc;
      if (deserializeJson(doc, resp.c_str(), resp.length()) == DeserializationError::Ok) {
        const char* url = doc["fulfill"]["url"] | doc["url"] | "";
        if (url[0] != '\0') {
          downloadUrl = url;
          sendBearer = false;
        }
      }
    }
    resp = String();

    std::vector<HttpDownloader::Header> dlHeaders;
    if (sendBearer) {
      dlHeaders.emplace_back("Authorization", "Bearer " + token);
    }
    const auto dlErr =
        HttpDownloader::downloadToFile(downloadUrl, destEpubPathOut, progressCb, nullptr, "", "", dlHeaders);
    if (dlErr != HttpDownloader::OK) {
      errOut = "Failed to download open EPUB from Libby";
      return false;
    }
  } else {
    String resp;
    const int status = pluginhttp::request(nullptr, fulfillEndpoint, "GET", "", authHeaders, resp, 24 * 1024);
    if (status < 200 || status >= 300 || resp.length() == 0) {
      errOut = "Failed to request Libby Adobe ACSM ticket (HTTP " + std::to_string(status) + ")";
      return false;
    }

    std::string acsmXml(resp.c_str(), resp.length());
    resp = String();

    if (!adept::isAcsmContent(acsmXml) && !acsmXml.empty() && acsmXml[0] == '{') {
      JsonDocument doc;
      if (deserializeJson(doc, acsmXml) == DeserializationError::Ok) {
        const char* acsmUrl = doc["fulfill"]["url"] | doc["url"] | "";
        if (acsmUrl[0] != '\0') {
          String acsmResp;
          const int acsmStatus = pluginhttp::request(nullptr, acsmUrl, "GET", "", {}, acsmResp, 24 * 1024);
          if (acsmStatus >= 200 && acsmStatus < 300 && acsmResp.length() > 0) {
            acsmXml.assign(acsmResp.c_str(), acsmResp.length());
          }
        }
      }
    }

    if (!adept::isAcsmContent(acsmXml)) {
      errOut = "Did not receive a valid .acsm ticket from OverDrive";
      return false;
    }

    if (!adept::fulfillAcsmContentToEpub(acsmXml, destEpubPathOut, errOut, progressCb)) {
      return false;
    }
  }

  const std::string sidecarPath = destEpubPathOut + ".meta.json";
  const std::string sidecarBody = serializeLoanSidecarJson(loan);
  Storage.writeFile(sidecarPath.c_str(), String(sidecarBody.c_str()));

  clearBookCache(destEpubPathOut);
  if (library::isLibraryBookFile(destEpubPathOut)) {
    library::markLibraryIndexDirty();
  }
  return true;
}

bool syncAndFulfillNewLoans(int& fulfilledCountOut, std::string& errOut, const std::function<bool()>& shouldStop) {
  fulfilledCountOut = 0;
  std::vector<Loan> loans;
  if (!fetchLoans(loans, errOut)) {
    return false;
  }

  for (const auto& loan : loans) {
    if (!loan.isDownloadable()) continue;
    const std::string expectedPath = loanDestEpubPath(loan);
    if (Storage.exists(expectedPath.c_str())) continue;
    if (shouldStop && shouldStop()) break;

    std::string actualPath;
    std::string dlErr;
    if (downloadLoan(loan, actualPath, dlErr)) {
      ++fulfilledCountOut;
    } else {
      LOG_ERR("LBY", "Auto-fulfill skipped '%s': %s", loan.title.c_str(), dlErr.c_str());
    }
  }
  return true;
}

}  // namespace libby
