#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;
class MappedInputManager;

namespace shibadesk {

// Draws the S1 Editorial Pill status bar (y = 0..37):
// - Left: black rounded pill [pillLabel] + optional `countSubtitle`
// - Right: live Wi-Fi pixel icon + 1px divider + battery % + 23x12 segmented pixel battery
//   (with <= 20% 1-bar + '!' warning state). Zero animal heads, zero paw prints, zero fake clock.
void drawShibaStatusBar(const GfxRenderer& renderer, const char* pillLabel, const char* countSubtitle);

// Draws the 23x12 segmented 1-bit pixel battery gauge at (x, y).
void drawShibaPixelBattery(const GfxRenderer& renderer, int x, int y, int percent, bool black = true);

// Draws the 4-capsule Home navigation dock (Library / Files / Sync / Settings).
// `focusedDockIdx` in [0..3] highlights the focused capsule; -1 means unfocused.
void drawShibaHomeDock(const GfxRenderer& renderer, int focusedDockIdx, const char* const labels[4],
                       Rect outSlotRects[4] = nullptr);

// Draws the unclipped Library pagination readout bar above the hardware key hints.
void drawShibaPaginationBar(const GfxRenderer& renderer, int currentPage, int totalPages, int startItem, int endItem,
                            int totalItems);

// Draws the 4-slot hardware key hint strip at the bottom of the screen, respecting
// `MappedInputManager::mapLabels` front-button remapping and hiding automatically on touch boards.
void drawShibaHardwareKeyHints(const GfxRenderer& renderer, const MappedInputManager& mappedInput,
                               const char* backLabel, const char* confirmLabel, const char* prevLabel,
                               const char* nextLabel);

// Draws pre-mapped 4-slot hardware key hints (used by ShibaDeskTheme::drawButtonHints).
void drawShibaMappedKeyHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                             const char* btn4);

}  // namespace shibadesk
