#pragma once

#include <TxtCoverMeta.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

class BookCoverStore {
 public:
  static constexpr int kCanonicalCoverWidth = 148;
  static constexpr int kCanonicalCoverHeight = 222;

  static std::string getBoundCoverBmpPath(std::string_view bookPath);
  static std::string getBoundMetaJsonPath(std::string_view bookPath);
  static bool hasBoundCover(std::string_view bookPath);
  static bool readBoundMeta(std::string_view bookPath, txtmeta::BoundBookMeta& out);

  // Validates a raw 1-bit uncompressed BMP header and pixel buffer bounds.
  static bool validate1BitBmpPayload(const uint8_t* data, size_t len, int* widthOut = nullptr,
                                     int* heightOut = nullptr);

  // Converts a JPG/PNG/BMP image on SD to a 148x222 1-bit Atkinson BMP under /.crosspoint/covers/<hash>.bmp,
  // optionally writes/updates <hash>.meta.json, invalidates stale cover/metadata caches while keeping
  // progress.bin intact, updates RECENT_BOOKS, and marks the library index dirty.
  static bool bindCoverAndMetaFromImageFile(const std::string& bookPath, const std::string& sourceImagePath,
                                            const std::string* optionalTitle = nullptr,
                                            const std::string* optionalAuthor = nullptr);

  // Binds a validated 1-bit BMP byte buffer (e.g. from the Web Upload client-side 1-bit Atkinson preview)
  // directly into /.crosspoint/covers/<hash>.bmp and optionally writes/updates <hash>.meta.json.
  static bool bindCoverAndMetaFrom1BitBmpBytes(const std::string& bookPath, const uint8_t* bmpData, size_t bmpLen,
                                               const std::string* optionalTitle = nullptr,
                                               const std::string* optionalAuthor = nullptr);

  // Writes/updates title & author in /.crosspoint/covers/<hash>.meta.json without changing the cover BMP.
  static bool bindMetadataOnly(const std::string& bookPath, const std::string& title, const std::string& author);

  // Clears the custom bound cover BMP (restoring the procedural cloth-spine cover or companion image)
  // while keeping progress.bin and .meta.json intact.
  static bool clearBoundCover(const std::string& bookPath);

  // Removes bound <hash>.bmp, <hash>.meta.json, and adjacent <bookPath>.meta.json when a book is deleted.
  static void removeBoundAssets(const std::string& bookPath);

  // Migrates bound <hash>.bmp, <hash>.meta.json, and adjacent <bookPath>.meta.json when a book is renamed/moved.
  static void moveBoundAssets(const std::string& oldBookPath, const std::string& newBookPath);

  // Invalidates cached thumb_*.bmp / cover*.bmp / cover.stamp (preserving progress.bin and content cache),
  // updates RECENT_BOOKS, and marks library.idx dirty.
  static void refreshBookStateAfterCoverOrMetaChange(const std::string& bookPath);

  // Converts an image file on SD into an exact-size 1-bit Atkinson BMP on SD (used by TxtCoverPickerActivity
  // preview and cover binding).
  static bool convertImageFileTo1BitBmp(const std::string& sourceImagePath, const std::string& destBmpPath,
                                        int targetWidth, int targetHeight);
};
