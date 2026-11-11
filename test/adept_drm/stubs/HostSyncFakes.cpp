#include "HostSyncFakes.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>

#include "BookKey.h"
#include "LibraryBuilder.h"
#include "ObfuscationUtils.h"
#include "PersistableStore.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/PluginHttp.h"

namespace hostnet {

std::vector<std::pair<std::string, std::string>>& httpQueue() {
  static std::vector<std::pair<std::string, std::string>> q;
  return q;
}

std::vector<std::string>& postBodies() {
  static std::vector<std::string> b;
  return b;
}

void reset() {
  hostfs::reset();
  httpQueue().clear();
  postBodies().clear();
}

namespace {

std::string base64Encode(const uint8_t* data, size_t len) {
  static constexpr char kTab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t b0 = data[i];
    const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
    const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
    const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(kTab[(triple >> 18) & 0x3Fu]);
    out.push_back(kTab[(triple >> 12) & 0x3Fu]);
    out.push_back((i + 1 < len) ? kTab[(triple >> 6) & 0x3Fu] : '=');
    out.push_back((i + 2 < len) ? kTab[triple & 0x3Fu] : '=');
  }
  return out;
}

bool base64Decode(std::string_view in, std::string& out) {
  static constexpr std::string_view kTab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  out.clear();
  if (in.empty()) return false;
  uint32_t acc = 0;
  unsigned bits = 0;
  for (char c : in) {
    if (c == '=') break;
    const size_t idx = kTab.find(c);
    if (idx == std::string_view::npos) return false;
    acc = (acc << 6u) | static_cast<uint32_t>(idx);
    bits += 6u;
    if (bits >= 8u) {
      bits -= 8u;
      out.push_back(static_cast<char>((acc >> bits) & 0xFFu));
    }
  }
  return true;
}

void xorWithTestMac(std::string& s) {
  static constexpr uint8_t kMac[6] = {0x3A, 0x7F, 0xC1, 0x19, 0x8E, 0x55};
  for (size_t i = 0; i < s.size(); ++i) {
    s[i] = static_cast<char>(static_cast<uint8_t>(s[i]) ^ kMac[i % 6]);
  }
}

}  // namespace

}  // namespace hostnet

namespace obfuscation {

String obfuscateToBase64(const std::string& plaintext) {
  if (plaintext.empty()) return {};
  std::string tmp = plaintext;
  hostnet::xorWithTestMac(tmp);
  return String(hostnet::base64Encode(reinterpret_cast<const uint8_t*>(tmp.data()), tmp.size()));
}

std::string deobfuscateFromBase64(const char* encoded, size_t maxDecodedLength, bool* ok, bool* tooLong) {
  if (tooLong) *tooLong = false;
  if (!encoded || encoded[0] == '\0') {
    if (ok) *ok = false;
    return {};
  }
  std::string decoded;
  if (!hostnet::base64Decode(encoded, decoded)) {
    if (ok) *ok = false;
    return {};
  }
  if (decoded.size() > maxDecodedLength) {
    if (ok) *ok = false;
    if (tooLong) *tooLong = true;
    return {};
  }
  hostnet::xorWithTestMac(decoded);
  if (ok) *ok = true;
  return decoded;
}

std::string deobfuscateFromBase64(const char* encoded, bool* ok) {
  return deobfuscateFromBase64(encoded, std::numeric_limits<size_t>::max(), ok, nullptr);
}

}  // namespace obfuscation

namespace bookkey {
bool write(const std::string& keyPath, const uint8_t* key, size_t len, int64_t) {
  hostfs::writeString(keyPath, std::string(reinterpret_cast<const char*>(key), len));
  return true;
}
}  // namespace bookkey

namespace library {
bool isLibraryBookFile(std::string_view) { return true; }
bool markLibraryIndexDirty() { return true; }
}  // namespace library

void clearBookCache(const std::string&) {}

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  if (!path) return false;
  std::string out;
  serializeJson(doc, out);
  hostfs::writeString(path, out);
  return true;
}

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  if (!path || !Storage.exists(path)) return false;
  const std::string raw = hostfs::readString(path);
  return !raw.empty() && deserializeJson(doc, raw) == DeserializationError::Ok;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string&, const std::string& destPath,
                                                             ProgressCallback, const bool*, const std::string&,
                                                             const std::string&, const std::vector<Header>&, bool) {
  hostfs::writeString(destPath, "DOWNLOADED_CONTENT");
  return OK;
}

namespace pluginhttp {

std::string escapeUrl(const std::string& url) { return url; }

std::string urlEncodeQuery(const std::string& value) {
  std::string out;
  char hex[4];
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      std::snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned>(c));
      out.append(hex);
    }
  }
  return out;
}

bool saveTokenToFile(const std::string& path, const std::string& body, const std::string&) {
  hostfs::writeString(path, body);
  return true;
}

int request(freeink::SecureHttpClient*, const std::string& url, const std::string& method, const std::string& body,
            const Headers&, String& respOut, size_t, Headers*, const std::function<bool()>&) {
  for (const auto& [needle, payload] : hostnet::httpQueue()) {
    if (url.find(needle) != std::string::npos) {
      respOut = String(payload);
      return 200;
    }
  }
  if (url.find("/getFile?file_id=") != std::string::npos) {
    const size_t eq = url.rfind('=');
    const std::string fid = (eq != std::string::npos) ? url.substr(eq + 1) : "file";
    respOut = String(std::string(R"json({"ok":true,"result":{"file_path":"remote/)json") + fid + R"json("}})json");
    return 200;
  }
  if (method == "POST" && url.find("/sendMessage") != std::string::npos) {
    hostnet::postBodies().push_back(body);
    respOut = String(R"json({"ok":true})json");
    return 200;
  }
  return -1;
}

int requestToFile(freeink::SecureHttpClient* session, const std::string& url, const std::string& method,
                  const std::string& body, const Headers& headers, const char* destPath, size_t maxResponse) {
  String resp;
  const int code = request(session, url, method, body, headers, resp, maxResponse);
  if (code >= 200 && code < 300 && destPath) {
    hostfs::writeString(destPath, std::string(resp.c_str(), resp.length()));
  }
  return code;
}

}  // namespace pluginhttp
