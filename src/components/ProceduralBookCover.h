#pragma once

#include <string>

class GfxRenderer;

class ProceduralBookCover {
 public:
  // Procedural 1-bit stitched cloth-spine cover for coverless .txt/.md books (148x222 hero, 102x153 shelf, etc.).
  static void drawProceduralTxtCover(const GfxRenderer& renderer, int x, int y, int w, int h, const std::string& title,
                                     const std::string& author, const char* extTag = nullptr);

  // Crisp miniature 1-bit cloth-spine cover for coverless .txt/.md rows in the Library list (52x78).
  static void drawMiniProceduralTxtCover(const GfxRenderer& renderer, int x, int y, int w, int h,
                                         const char* extTag = "TXT");
};
