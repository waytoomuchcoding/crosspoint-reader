#include "AdeptFulfiller.h"

#include <ArduinoJson.h>
#include <BookKey.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <ProtectedBook.h>
#include <Util.h>
#include <WolfsslCrypto.h>
#include <expat.h>
#include <strings.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "network/HttpDownloader.h"
#include "util/Iso8601.h"
#include "util/PluginHttp.h"

namespace adept {

namespace {

constexpr char kAdeptNs[] = "http://ns.adobe.com/adept";

std::string base64EncodeBytes(const uint8_t* data, size_t len) {
  static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t b0 = data[i];
    const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
    const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
    const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(kTable[(triple >> 18) & 0x3F]);
    out.push_back(kTable[(triple >> 12) & 0x3F]);
    out.push_back((i + 1 < len) ? kTable[(triple >> 6) & 0x3F] : '=');
    out.push_back((i + 2 < len) ? kTable[triple & 0x3F] : '=');
  }
  return out;
}

std::string trimAsciiWhitespace(std::string_view sv) {
  size_t start = 0;
  while (start < sv.size() && std::isspace(static_cast<unsigned char>(sv[start]))) {
    start++;
  }
  size_t end = sv.size();
  while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1]))) {
    end--;
  }
  return std::string(sv.substr(start, end - start));
}

struct AdeptAttr {
  std::string ns;
  std::string name;
  std::string value;
};

struct AdeptNode {
  std::string ns;
  std::string name;
  std::vector<AdeptAttr> attrs;
  std::string text;
  std::vector<AdeptNode> children;
};

void splitNsName(const char* expatName, std::string& nsOut, std::string& localOut) {
  const char* bar = strchr(expatName, '|');
  if (bar) {
    nsOut.assign(expatName, static_cast<size_t>(bar - expatName));
    localOut.assign(bar + 1);
  } else {
    nsOut.clear();
    localOut.assign(expatName);
  }
}

struct TreeBuilder {
  std::vector<AdeptNode*> stack;
  AdeptNode root;
  bool hasRoot = false;
};

void XMLCALL onTreeStart(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* tb = static_cast<TreeBuilder*>(userData);
  AdeptNode node;
  splitNsName(name, node.ns, node.name);

  if (atts) {
    for (int i = 0; atts[i]; i += 2) {
      AdeptAttr attr;
      splitNsName(atts[i], attr.ns, attr.name);
      attr.value = atts[i + 1];
      node.attrs.push_back(std::move(attr));
    }
    std::sort(node.attrs.begin(), node.attrs.end(), [](const AdeptAttr& a, const AdeptAttr& b) {
      if (a.ns != b.ns) return a.ns < b.ns;
      return a.name < b.name;
    });
  }

  if (tb->stack.empty()) {
    tb->root = std::move(node);
    tb->hasRoot = true;
    tb->stack.push_back(&tb->root);
  } else {
    AdeptNode* parent = tb->stack.back();
    parent->children.push_back(std::move(node));
    tb->stack.push_back(&parent->children.back());
  }
}

void XMLCALL onTreeEnd(void* userData, const XML_Char* /*name*/) {
  auto* tb = static_cast<TreeBuilder*>(userData);
  if (!tb->stack.empty()) {
    tb->stack.pop_back();
  }
}

void XMLCALL onTreeChar(void* userData, const XML_Char* s, int len) {
  auto* tb = static_cast<TreeBuilder*>(userData);
  if (!tb->stack.empty() && len > 0) {
    tb->stack.back()->text.append(s, static_cast<size_t>(len));
  }
}

bool parseXmlTree(std::string_view xml, TreeBuilder& tb) {
  XML_Parser parser = XML_ParserCreateNS(nullptr, '|');
  if (!parser) return false;
  XML_SetUserData(parser, &tb);
  XML_SetElementHandler(parser, onTreeStart, onTreeEnd);
  XML_SetCharacterDataHandler(parser, onTreeChar);
  const bool ok = (XML_Parse(parser, xml.data(), static_cast<int>(xml.size()), 1) == XML_STATUS_OK) && tb.hasRoot;
  XML_ParserFree(parser);
  return ok;
}

void appendU8(std::vector<uint8_t>& buf, uint8_t v) { buf.push_back(v); }

void appendStringWithLen(std::vector<uint8_t>& buf, const std::string& s) {
  const uint16_t len = static_cast<uint16_t>(std::min<size_t>(s.size(), 0xFFFF));
  buf.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>(len & 0xFF));
  buf.insert(buf.end(), s.begin(), s.begin() + len);
}

void serializeAdeptNode(const AdeptNode& node, bool isRoot, std::vector<uint8_t>& out) {
  if (isRoot && node.name == "signature" && node.ns == kAdeptNs) {
    return;
  }
  appendU8(out, 0x01);
  appendStringWithLen(out, node.ns);
  appendStringWithLen(out, node.name);

  for (const auto& attr : node.attrs) {
    appendU8(out, 0x02);
    appendStringWithLen(out, attr.ns);
    appendStringWithLen(out, attr.name);
    appendStringWithLen(out, attr.value);
  }

  appendU8(out, 0x03);

  const std::string trimmed = trimAsciiWhitespace(node.text);
  if (!trimmed.empty()) {
    size_t offset = 0;
    while (offset < trimmed.size()) {
      const size_t chunk = std::min<size_t>(trimmed.size() - offset, 0x7FFF);
      appendU8(out, 0x04);
      appendStringWithLen(out, trimmed.substr(offset, chunk));
      offset += chunk;
    }
  }

  for (const auto& child : node.children) {
    if (isRoot && child.name == "signature" && child.ns == kAdeptNs) {
      continue;
    }
    serializeAdeptNode(child, /*isRoot=*/false, out);
  }

  appendU8(out, 0x05);
}

void hashCanonicalTree(const AdeptNode& root, size_t sizeHint, uint8_t sha1Out[20]) {
  std::vector<uint8_t> canonical;
  canonical.reserve(sizeHint);
  serializeAdeptNode(root, /*isRoot=*/true, canonical);
  freeink::content::WolfsslCrypto crypto;
  crypto.sha1(canonical.data(), canonical.size(), sha1Out);
}

AdeptNode makeSimpleAdeptElement(const char* localName, std::string_view text) {
  AdeptNode n;
  n.ns = kAdeptNs;
  n.name = localName;
  n.text.assign(text.data(), text.size());
  return n;
}

const AdeptNode* findChild(const AdeptNode& parent, std::string_view localName) {
  for (const auto& child : parent.children) {
    if (child.name == localName) {
      return &child;
    }
  }
  return nullptr;
}

const AdeptNode* findDescendant(const AdeptNode& node, std::string_view localName) {
  if (node.name == localName) return &node;
  for (const auto& child : node.children) {
    if (const AdeptNode* found = findDescendant(child, localName)) {
      return found;
    }
  }
  return nullptr;
}

void appendXmlEscaped(std::string& out, std::string_view text) {
  for (char c : text) {
    switch (c) {
      case '&':
        out.append("&amp;");
        break;
      case '<':
        out.append("&lt;");
        break;
      case '>':
        out.append("&gt;");
        break;
      case '"':
        out.append("&quot;");
        break;
      default:
        out.push_back(c);
        break;
    }
  }
}

}  // namespace

bool isAcsmContent(std::string_view content) {
  const std::string trimmed = trimAsciiWhitespace(content);
  return trimmed.find("<fulfillmentToken") != std::string::npos;
}

bool isAcsmFile(const std::string& path) {
  if (path.size() >= 5 && strcasecmp(path.c_str() + path.size() - 5, ".acsm") == 0) {
    return true;
  }
  HalFile f;
  if (!Storage.openFileForRead("ACSM", path, f)) {
    return false;
  }
  char head[160] = {0};
  const int n = f.read(head, sizeof(head) - 1);
  f.close();
  if (n <= 0) return false;
  return isAcsmContent(std::string_view(head, static_cast<size_t>(n)));
}

bool computeAdeptXmlSha1(std::string_view xmlFragment, uint8_t sha1Out[20]) {
  TreeBuilder tb;
  if (!parseXmlTree(xmlFragment, tb)) {
    return false;
  }
  hashCanonicalTree(tb.root, xmlFragment.size(), sha1Out);
  return true;
}

bool buildSignedFulfillRequest(std::string_view acsmXml, const DeviceCredentials& creds, std::string& operatorUrlOut,
                               std::string& signedFulfillXmlOut, std::string& errOut) {
  operatorUrlOut.clear();
  signedFulfillXmlOut.clear();

  if (creds.userUrn.empty() || creds.deviceUrn.empty() || creds.privateKeyDer.empty()) {
    errOut = "Missing Adobe ADEPT device credentials";
    return false;
  }

  TreeBuilder tb;
  if (!parseXmlTree(acsmXml, tb) || tb.root.name != "fulfillmentToken") {
    errOut = "Invalid .acsm fulfillmentToken XML";
    return false;
  }

  const AdeptNode* opNode = findChild(tb.root, "operatorURL");
  if (!opNode || trimAsciiWhitespace(opNode->text).empty()) {
    errOut = "Missing <operatorURL> in .acsm";
    return false;
  }
  operatorUrlOut = trimAsciiWhitespace(opNode->text);
  while (!operatorUrlOut.empty() && operatorUrlOut.back() == '/') {
    operatorUrlOut.pop_back();
  }
  if (operatorUrlOut.size() < 8 || operatorUrlOut.substr(operatorUrlOut.size() - 8) != "/Fulfill") {
    operatorUrlOut += "/Fulfill";
  }

  const size_t ftStart = acsmXml.find("<fulfillmentToken");
  const size_t ftEnd = acsmXml.rfind("</fulfillmentToken>");
  if (ftStart == std::string_view::npos || ftEnd == std::string_view::npos || ftEnd < ftStart) {
    errOut = "Malformed <fulfillmentToken> tags in .acsm";
    return false;
  }
  const std::string_view rawFulfillmentToken =
      acsmXml.substr(ftStart, (ftEnd + strlen("</fulfillmentToken>")) - ftStart);

  const std::string_view devType = creds.deviceType.empty() ? std::string_view("standalone") : creds.deviceType;

  // Build the canonical <fulfill> tree directly around the already-parsed <fulfillmentToken> node
  // so we do not invoke Expat a second time over the full document.
  AdeptNode fulfillTree;
  fulfillTree.ns = kAdeptNs;
  fulfillTree.name = "fulfill";
  fulfillTree.children.reserve(4);
  fulfillTree.children.push_back(makeSimpleAdeptElement("user", creds.userUrn));
  fulfillTree.children.push_back(makeSimpleAdeptElement("device", creds.deviceUrn));
  fulfillTree.children.push_back(makeSimpleAdeptElement("deviceType", devType));
  fulfillTree.children.push_back(std::move(tb.root));

  uint8_t digest[20];
  hashCanonicalTree(fulfillTree, acsmXml.size() + 256, digest);
  fulfillTree = {};

  freeink::content::WolfsslCrypto crypto;
  uint8_t sig[128];
  if (!crypto.rsaPrivateSignRaw(creds.privateKeyDer.data(), creds.privateKeyDer.size(), digest, sig)) {
    errOut = "RSA signature failed for <fulfill> request";
    return false;
  }

  const std::string sigB64 = base64EncodeBytes(sig, sizeof(sig));
  signedFulfillXmlOut.reserve(rawFulfillmentToken.size() + sigB64.size() + 256);
  signedFulfillXmlOut = "<?xml version=\"1.0\"?>\n<fulfill xmlns=\"http://ns.adobe.com/adept\">\n  <user>";
  appendXmlEscaped(signedFulfillXmlOut, creds.userUrn);
  signedFulfillXmlOut += "</user>\n  <device>";
  appendXmlEscaped(signedFulfillXmlOut, creds.deviceUrn);
  signedFulfillXmlOut += "</device>\n  <deviceType>";
  appendXmlEscaped(signedFulfillXmlOut, devType);
  signedFulfillXmlOut += "</deviceType>\n  ";
  signedFulfillXmlOut.append(rawFulfillmentToken.data(), rawFulfillmentToken.size());
  signedFulfillXmlOut += "\n  <signature>";
  signedFulfillXmlOut += sigB64;
  signedFulfillXmlOut += "</signature>\n</fulfill>";
  return true;
}

int64_t parseIso8601Timestamp(std::string_view iso) { return iso8601::parseUtc(iso); }

bool parseFulfillmentResultXml(std::string_view responseXml, FulfillmentReply& replyOut, std::string& errOut) {
  replyOut = {};
  TreeBuilder tb;
  if (!parseXmlTree(responseXml, tb)) {
    errOut = "Malformed XML response from Adobe ACS server";
    return false;
  }

  if (tb.root.name == "error") {
    for (const auto& attr : tb.root.attrs) {
      if (attr.name == "data") {
        replyOut.errorCode = attr.value;
        break;
      }
    }
    errOut = replyOut.errorCode.empty() ? "Adobe ACS returned <error>" : ("Adobe ACS error: " + replyOut.errorCode);
    return false;
  }

  if (const AdeptNode* srcNode = findDescendant(tb.root, "src")) {
    replyOut.downloadUrl = trimAsciiWhitespace(srcNode->text);
  }
  if (const AdeptNode* keyNode = findDescendant(tb.root, "encryptedKey")) {
    replyOut.encryptedKeyB64 = trimAsciiWhitespace(keyNode->text);
  }
  if (const AdeptNode* untilNode = findDescendant(tb.root, "until")) {
    replyOut.expiresAt = parseIso8601Timestamp(untilNode->text);
  }

  if (replyOut.downloadUrl.empty() || replyOut.encryptedKeyB64.empty()) {
    errOut = "Missing <src> or <encryptedKey> in <fulfillmentResult>";
    return false;
  }
  return true;
}

bool extractPkcs1v15AesKey(const uint8_t* plain, const size_t plainLen, uint8_t aesKeyOut[16]) {
  if (!plain || !aesKeyOut || plainLen < 26) return false;

  size_t padStart = 0;
  if (plain[0] == 0x00 && plain[1] == 0x02) {
    padStart = 2;
  } else if (plain[0] == 0x02) {
    // Some raw RSA implementations strip the leading 0x00 octet from big-endian output.
    padStart = 1;
  } else {
    return false;
  }

  for (size_t i = padStart; i < plainLen; ++i) {
    if (plain[i] == 0x00) {
      const size_t padBytes = i - padStart;
      const size_t payloadBytes = plainLen - (i + 1);
      if (padBytes < 8 || payloadBytes != 16) {
        return false;
      }
      std::memcpy(aesKeyOut, plain + i + 1, 16);
      return true;
    }
  }
  return false;
}

bool unwrapEncryptedBookKey(std::string_view encryptedKeyB64, const std::vector<uint8_t>& privateKeyDer,
                            uint8_t aesKeyOut[16], std::string& errOut) {
  if (encryptedKeyB64.empty() || privateKeyDer.empty()) {
    errOut = "Empty encrypted key or device private key";
    return false;
  }

  // Heap-allocate the 2x256B RSA cipher/plaintext buffers to keep task stack usage < 256B.
  static constexpr size_t kRsaBufCap = 256;
  auto scratch = makeUniqueNoThrow<uint8_t[]>(kRsaBufCap * 2);
  if (!scratch) {
    LOG_ERR("ACSM", "OOM: RSA unwrap scratch (%zu B)", kRsaBufCap * 2);
    errOut = "Out of memory unwrapping book key";
    return false;
  }
  uint8_t* cipher = scratch.get();
  uint8_t* plain = scratch.get() + kRsaBufCap;

  const int32_t cipherLen =
      freeink::content::base64Decode(encryptedKeyB64.data(), encryptedKeyB64.size(), cipher, kRsaBufCap);
  if (cipherLen <= 0) {
    errOut = "Failed to base64-decode <encryptedKey>";
    return false;
  }

  freeink::content::WolfsslCrypto crypto;
  const int32_t plainLen = crypto.rsaPrivateRaw(privateKeyDer.data(), privateKeyDer.size(), cipher,
                                                static_cast<size_t>(cipherLen), plain, kRsaBufCap);
  if (plainLen <= 0) {
    std::memset(scratch.get(), 0, kRsaBufCap * 2);
    errOut = "RSA private-key decrypt failed on <encryptedKey>";
    return false;
  }

  const bool unwrapped = extractPkcs1v15AesKey(plain, static_cast<size_t>(plainLen), aesKeyOut);
  std::memset(scratch.get(), 0, kRsaBufCap * 2);
  if (!unwrapped) {
    errOut = "Invalid PKCS#1 v1.5 padding in decrypted book key";
    return false;
  }
  return true;
}

bool loadDeviceCredentials(DeviceCredentials& credsOut, std::string& errOut) {
  credsOut = {};
  static constexpr const char* kCandidateJsonPaths[] = {
      "/.crosspoint/adept.json",
      "/.crosspoint/plugins/protected-content/adept.json",
      "/.crosspoint/plugins/protected-content/config.json",
      "/.crosspoint/plugins/protected-content/activation.json",
      "/plugins/protected-content/adept.json",
      "/plugins/protected-content/config.json",
      "/plugins/protected-content/activation.json",
  };

  for (const char* path : kCandidateJsonPaths) {
    if (!Storage.exists(path)) continue;
    std::string jsonText;
    if (!Storage.readFileToString("ACSM", path, 16 * 1024, jsonText)) continue;

    JsonDocument doc;
    if (deserializeJson(doc, jsonText) != DeserializationError::Ok) continue;

    const char* user = doc["user"] | doc["userUrn"] | doc["user_urn"] | "";
    const char* dev = doc["device"] | doc["deviceUrn"] | doc["device_urn"] | doc["deviceId"] | "";
    const char* keyB64 =
        doc["privateKey"] | doc["private_key"] | doc["deviceKey"] | doc["device_key"] | doc["key"] | "";
    const char* devType = doc["deviceType"] | doc["device_type"] | "standalone";

    if (user[0] != '\0' && dev[0] != '\0' && keyB64[0] != '\0') {
      const size_t b64Len = strlen(keyB64);
      std::vector<uint8_t> der(b64Len * 3 / 4 + 4);
      const int32_t n = freeink::content::base64Decode(keyB64, b64Len, der.data(), der.size());
      if (n > 0) {
        der.resize(static_cast<size_t>(n));
        credsOut.userUrn = user;
        credsOut.deviceUrn = dev;
        credsOut.deviceType = devType;
        credsOut.privateKeyDer = std::move(der);
        return true;
      }
    }
  }

  // Also check XML activation files (`/.crosspoint/adept/activation.xml` + `/.crosspoint/plugins/protected-content/*`)
  static constexpr const char* kActivationXmlPaths[] = {
      "/.crosspoint/adept/activation.xml",
      "/.crosspoint/plugins/protected-content/activation.xml",
      "/plugins/protected-content/activation.xml",
  };
  for (const char* xmlPath : kActivationXmlPaths) {
    if (!Storage.exists(xmlPath)) continue;
    std::string xmlText;
    if (!Storage.readFileToString("ACSM", xmlPath, 24 * 1024, xmlText)) continue;

    TreeBuilder tb;
    if (!parseXmlTree(xmlText, tb)) continue;

    if (const AdeptNode* u = findDescendant(tb.root, "user")) credsOut.userUrn = trimAsciiWhitespace(u->text);
    if (const AdeptNode* d = findDescendant(tb.root, "device")) credsOut.deviceUrn = trimAsciiWhitespace(d->text);
    const AdeptNode* pk = findDescendant(tb.root, "privateLicenseKey");
    if (!pk) pk = findDescendant(tb.root, "privateKey");
    if (pk) {
      const std::string b64 = trimAsciiWhitespace(pk->text);
      std::vector<uint8_t> der(b64.size() * 3 / 4 + 4);
      const int32_t n = freeink::content::base64Decode(b64.data(), b64.size(), der.data(), der.size());
      if (n > 0) {
        der.resize(static_cast<size_t>(n));
        credsOut.privateKeyDer = std::move(der);
      }
    }
    if (!credsOut.userUrn.empty() && !credsOut.deviceUrn.empty() && !credsOut.privateKeyDer.empty()) {
      return true;
    }
  }

  errOut = "No Adobe ADEPT activation found on SD card (activate in Protected Content plugin first)";
  return false;
}

bool fulfillAcsmContentToEpub(std::string_view acsmXml, const std::string& destEpubPath, std::string& errOut,
                              std::function<void(size_t, size_t)> progressCb) {
  DeviceCredentials creds;
  if (!loadDeviceCredentials(creds, errOut)) {
    return false;
  }

  std::string operatorUrl;
  std::string signedFulfillXml;
  if (!buildSignedFulfillRequest(acsmXml, creds, operatorUrl, signedFulfillXml, errOut)) {
    return false;
  }

  pluginhttp::Headers headers = {
      {"Content-Type", "application/vnd.adobe.adept+xml"},
      {"Accept", "*/*"},
      {"User-Agent", "book2png"},
  };
  String responseBody;
  const int status =
      pluginhttp::request(nullptr, operatorUrl, "POST", signedFulfillXml, headers, responseBody, 32 * 1024);
  if (status < 200 || status >= 300 || responseBody.length() == 0) {
    errOut = "Adobe ACS Fulfill POST failed (HTTP " + std::to_string(status) + ")";
    return false;
  }

  FulfillmentReply reply;
  if (!parseFulfillmentResultXml(std::string_view(responseBody.c_str(), responseBody.length()), reply, errOut)) {
    return false;
  }
  responseBody = String();  // Reclaim DRAM before downloading the EPUB

  uint8_t aesKey[16];
  if (!unwrapEncryptedBookKey(reply.encryptedKeyB64, creds.privateKeyDer, aesKey, errOut)) {
    return false;
  }

  const auto dlErr = HttpDownloader::downloadToFile(reply.downloadUrl, destEpubPath, progressCb, nullptr, "", "",
                                                    {{"User-Agent", "book2png"}}, /*downgradeRedirectsToHttp=*/true);
  if (dlErr != HttpDownloader::OK) {
    std::memset(aesKey, 0, sizeof(aesKey));
    errOut = "Failed to download fulfilled EPUB from " + reply.downloadUrl;
    return false;
  }

  const bool wroteKey = bookkey::write(destEpubPath, aesKey, sizeof(aesKey), reply.expiresAt);
  std::memset(aesKey, 0, sizeof(aesKey));
  if (!wroteKey) {
    Storage.remove(destEpubPath.c_str());
    errOut = "Failed to write wrapped .epub.key file";
    return false;
  }

  LOG_INF("ACSM", "Successfully fulfilled ACSM to %s (expires=%lld)", destEpubPath.c_str(),
          static_cast<long long>(reply.expiresAt));
  return true;
}

bool fulfillAcsmFile(const std::string& acsmPath, std::string& finalEpubPathOut, std::string& errOut,
                     std::function<void(size_t, size_t)> progressCb) {
  std::string acsmXml;
  if (!Storage.readFileToString("ACSM", acsmPath, 24 * 1024, acsmXml)) {
    errOut = "Could not read .acsm file: " + acsmPath;
    return false;
  }

  if (finalEpubPathOut.empty()) {
    if (acsmPath.size() > 5 && strcasecmp(acsmPath.c_str() + acsmPath.size() - 5, ".acsm") == 0) {
      finalEpubPathOut = acsmPath.substr(0, acsmPath.size() - 5) + ".epub";
    } else {
      finalEpubPathOut = acsmPath;
    }
  }

  const std::string stagedEpub = (finalEpubPathOut == acsmPath) ? (acsmPath + ".epub.tmp") : finalEpubPathOut;
  if (!fulfillAcsmContentToEpub(acsmXml, stagedEpub, errOut, progressCb)) {
    return false;
  }

  if (stagedEpub != finalEpubPathOut) {
    if (!Storage.replaceFile(stagedEpub.c_str(), finalEpubPathOut.c_str())) {
      Storage.remove(stagedEpub.c_str());
      errOut = "Failed to replace .acsm file with fulfilled .epub";
      return false;
    }
  } else if (acsmPath != finalEpubPathOut) {
    Storage.remove(acsmPath.c_str());
  }

  return true;
}

}  // namespace adept
