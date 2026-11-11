#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace txtmeta {

struct BoundBookMeta {
  std::string title;
  std::string author;
  uint32_t updatedAt = 0;
};

// Returns true for .txt or .md paths (ASCII case-insensitive).
bool isTxtOrMdPath(std::string_view path);

// Normalizes a book path ("//Books/./novel.txt" -> "/Books/novel.txt") for deterministic hashing.
std::string canonicalizeBookPath(std::string_view bookPath);

// Deterministic 64-bit FNV-1a hex digest (16 lowercase hex digits) of the canonical book path.
std::string computePathHashHex(std::string_view bookPath);

constexpr const char* kHiddenCoversDir = "/.crosspoint/covers";

std::string getBoundCoverBmpPath(std::string_view bookPath);
std::string getBoundMetaJsonPath(std::string_view bookPath);

// Reads/writes {"title":"...","author":"...","updatedAt":...}.
// Returns false on missing file, malformed JSON, or I/O failure.
// When ioFailedOut != nullptr, *ioFailedOut is set true only if an existing file could not be read.
bool readMetaJson(const std::string& metaPath, BoundBookMeta& out, bool* ioFailedOut = nullptr);
bool writeMetaJson(const std::string& metaPath, const BoundBookMeta& meta);

// Splits a filename stem of the form "Title - Author" (or "Title — Author") on the last separator
// when both trimmed sides are non-empty.
bool parseTitleAuthorFromStem(std::string_view stem, std::string& titleOut, std::string& authorOut);

}  // namespace txtmeta
