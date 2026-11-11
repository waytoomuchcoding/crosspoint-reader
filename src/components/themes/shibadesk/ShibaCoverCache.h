#pragma once

#include <functional>
#include <string>

class GfxRenderer;

namespace shibadesk {

// Returns "MD", "TXT", "XTC", or "EPUB" for display in format badges.
std::string detectBookFormatTag(const std::string& path);

// Fast filesystem-only lookup for an already-cached exact-size thumbnail BMP
// without constructing Epub/Xtc container objects.
std::string lookupExistingThumbBmp(const std::string& bookPath, int targetW, int targetH);

// Resolves or generates an exact-size 1-bit thumbnail BMP on SD. Calls `beforeHeavyIo` /
// `afterHeavyIo` around container extraction when generation is actually required.
std::string generateThumbBmp(const std::string& bookPath, int targetW, int targetH,
                             const std::function<void()>& beforeHeavyIo = nullptr,
                             const std::function<void()>& afterHeavyIo = nullptr);

// Draws a cached thumbnail BMP into `(x, y, w, h)` using heap-allocated HalFile + Bitmap
// scratch objects so the caller's stack frame stays well under 256 bytes.
bool drawThumbBmp(GfxRenderer& renderer, const std::string& bmpPath, int x, int y, int w, int h);

}  // namespace shibadesk
