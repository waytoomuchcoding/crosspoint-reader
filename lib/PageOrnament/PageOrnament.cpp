#include "PageOrnament.h"

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace page_ornament {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kGoldenAngle = 2.39996322972865332f;  // 137.50776 deg

// Minimum sacred white-paper halo around all rendered ink (glyphs, ruby, rules, images, chrome).
constexpr float kInkHaloPx = 17.0f;
constexpr float kScreenEdgePadPx = 9.0f;

// Coarse grid cell size for fast Chamfer distance transform (4x4 px = ~26 KB on 528x792).
constexpr int kCellPx = 4;

// Strip height for 1:1 pixel tonal + knockout + Atkinson rasterization (~50 KB peak scratch).
constexpr int kStripRows = 48;

// Fast deterministic PRNG (xorshift64* / splitmix32).
struct Rng {
  uint32_t state;
  explicit Rng(uint32_t seed) : state(seed ? seed : 0x9E3779B9u) {}

  uint32_t nextU32() {
    uint32_t z = (state += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
  }

  float nextFloat() { return static_cast<float>(nextU32() & 0x00FFFFFFu) / 16777216.0f; }
  float range(float lo, float hi) { return lo + (hi - lo) * nextFloat(); }
  int rangeInt(int lo, int hiInclusive) {
    if (hiInclusive <= lo) return lo;
    return lo + static_cast<int>(nextU32() % static_cast<uint32_t>(hiInclusive - lo + 1));
  }
  bool chance(float p) { return nextFloat() < p; }
};

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothstepf(float e0, float e1, float x) {
  if (e1 <= e0) return x >= e1 ? 1.0f : 0.0f;
  const float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Deterministic 2D value noise + FBM for lunar maria, mountain mist, and petal texture.
inline float hash2i(int ix, int iy, uint32_t seed) {
  uint32_t h = static_cast<uint32_t>(ix) * 374761393u + static_cast<uint32_t>(iy) * 668265263u + seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return static_cast<float>((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
}

float valueNoise2D(float x, float y, uint32_t seed) {
  const int ix = static_cast<int>(std::floor(x));
  const int iy = static_cast<int>(std::floor(y));
  const float fx = x - static_cast<float>(ix);
  const float fy = y - static_cast<float>(iy);
  const float ux = fx * fx * (3.0f - 2.0f * fx);
  const float uy = fy * fy * (3.0f - 2.0f * fy);
  const float n00 = hash2i(ix, iy, seed);
  const float n10 = hash2i(ix + 1, iy, seed);
  const float n01 = hash2i(ix, iy + 1, seed);
  const float n11 = hash2i(ix + 1, iy + 1, seed);
  return lerpf(lerpf(n00, n10, ux), lerpf(n01, n11, ux), uy);
}

float fbm2D(float x, float y, uint32_t seed) {
  float sum = 0.0f;
  float amp = 0.55f;
  float freq = 1.0f;
  for (int i = 0; i < 3; ++i) {
    sum += amp * valueNoise2D(x * freq, y * freq, seed + static_cast<uint32_t>(i * 97));
    freq *= 2.07f;
    amp *= 0.48f;
  }
  return clampf(sum / 0.94f, 0.0f, 1.0f);
}

// Mirrors GfxRenderer::drawPixel logical-to-physical coordinate transform.
inline bool readLogicalInkBit(const GfxRenderer& renderer, const uint8_t* fb, int x, int y) {
  const uint16_t panelW = renderer.getDisplayWidth();
  const uint16_t panelH = renderer.getDisplayHeight();
  const uint16_t panelWB = renderer.getDisplayWidthBytes();
  int phyX = 0;
  int phyY = 0;
  switch (renderer.getOrientation()) {
    case GfxRenderer::Portrait:
      phyX = y;
      phyY = panelH - 1 - x;
      break;
    case GfxRenderer::LandscapeClockwise:
      phyX = panelW - 1 - x;
      phyY = panelH - 1 - y;
      break;
    case GfxRenderer::PortraitInverted:
      phyX = panelW - 1 - y;
      phyY = x;
      break;
    case GfxRenderer::LandscapeCounterClockwise:
      phyX = x;
      phyY = y;
      break;
  }
  if (phyX < 0 || phyX >= panelW || phyY < 0 || phyY >= panelH) return false;
  const uint32_t byteIdx = static_cast<uint32_t>(phyY) * panelWB + static_cast<uint32_t>(phyX >> 3);
  const uint8_t bitMask = static_cast<uint8_t>(1u << (7 - (phyX & 7)));
  // In BW framebuffer: 0 = black ink, 1 = white paper.
  return (fb[byteIdx] & bitMask) == 0;
}

// ============================================================================
// Phase 1: Dead-Space Distance Field & Topological Region Classifier
// ============================================================================

struct ClearanceField {
  int screenW = 0;
  int screenH = 0;
  int usableBottom = 0;
  int gw = 0;
  int gh = 0;
  float haloPx = kInkHaloPx;
  float edgePadPx = kScreenEdgePadPx;
  std::unique_ptr<uint8_t[]> inkDistPx;  // Distance to nearest occupied cell in pixels (0..255)

  float sampleInkDist(float x, float y) const {
    const float gx = clampf(x / static_cast<float>(kCellPx) - 0.5f, 0.0f, static_cast<float>(gw - 1));
    const float gy = clampf(y / static_cast<float>(kCellPx) - 0.5f, 0.0f, static_cast<float>(gh - 1));
    const int x0 = static_cast<int>(gx);
    const int y0 = static_cast<int>(gy);
    const int x1 = std::min(x0 + 1, gw - 1);
    const int y1 = std::min(y0 + 1, gh - 1);
    const float tx = gx - static_cast<float>(x0);
    const float ty = gy - static_cast<float>(y0);
    const float d00 = inkDistPx[y0 * gw + x0];
    const float d10 = inkDistPx[y0 * gw + x1];
    const float d01 = inkDistPx[y1 * gw + x0];
    const float d11 = inkDistPx[y1 * gw + x1];
    return lerpf(lerpf(d00, d10, tx), lerpf(d01, d11, tx), ty);
  }

  float clearanceAt(float x, float y) const {
    if (x < edgePadPx || x >= static_cast<float>(screenW) - edgePadPx) return -10.0f;
    if (y < edgePadPx || y >= static_cast<float>(usableBottom) - 4.0f) return -10.0f;
    const float edgeDist = std::min({x - edgePadPx, static_cast<float>(screenW - 1) - edgePadPx - x, y - edgePadPx,
                                     static_cast<float>(usableBottom) - 4.0f - y});
    const float inkClear = sampleInkDist(x, y) - haloPx;
    return std::min(inkClear, edgeDist);
  }

  void gradientAt(float x, float y, float& gxOut, float& gyOut) const {
    const float dx = clearanceAt(x + 4.0f, y) - clearanceAt(x - 4.0f, y);
    const float dy = clearanceAt(x, y + 4.0f) - clearanceAt(x, y - 4.0f);
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > 1e-4f) {
      gxOut = dx / len;
      gyOut = dy / len;
    } else {
      gxOut = 0.0f;
      gyOut = 0.0f;
    }
  }
};

enum class RegionKind : uint8_t {
  TailpieceExpanse,   // Open expanse below the last text/element block
  CrownExpanse,       // Open sinkage expanse near top of page
  MarginalBay,        // Tall asymmetric side channel (e.g. beside verse/poetry/dialogue)
  SceneBreakCorridor  // Mid-page horizontal corridor between two text blocks
};

struct Pole {
  float x = 0.0f;
  float y = 0.0f;
  float clearance = 0.0f;
};

struct DeadRegion {
  RegionKind kind = RegionKind::TailpieceExpanse;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  int areaPx = 0;
  std::vector<Pole> poles;  // Sorted descending by clearance
  int componentId = 0;
};

bool buildClearanceField(const GfxRenderer& renderer, const Page& page, const RenderContext& ctx,
                         ClearanceField& outField, int& outContentTop, int& outContentBottom) {
  const uint8_t* fb = renderer.getFrameBuffer();
  if (!fb) return false;

  const int W = renderer.getScreenWidth();
  const int H = renderer.getScreenHeight();
  if (W <= 64 || H <= 64) return false;

  const int gw = (W + kCellPx - 1) / kCellPx;
  const int gh = (H + kCellPx - 1) / kCellPx;
  const size_t totalCells = static_cast<size_t>(gw) * gh;

  auto dist16 = makeUniqueNoThrow<uint16_t[]>(totalCells);
  outField.inkDistPx = makeUniqueNoThrow<uint8_t[]>(totalCells);
  if (!dist16 || !outField.inkDistPx) return false;

  outField.screenW = W;
  outField.screenH = H;
  outField.gw = gw;
  outField.gh = gh;
  // Reserve status bar zone at bottom.
  outField.usableBottom = std::max(64, H - std::max(ctx.marginBottom, 12));

  constexpr uint16_t kInf = 30000;
  for (size_t i = 0; i < totalCells; ++i) dist16[i] = kInf;

  auto markRectOccupied = [&](int rx0, int ry0, int rx1, int ry1) {
    const int gx0 = std::max(0, rx0 / kCellPx);
    const int gy0 = std::max(0, ry0 / kCellPx);
    const int gx1 = std::min(gw - 1, std::max(0, rx1) / kCellPx);
    const int gy1 = std::min(gh - 1, std::max(0, ry1) / kCellPx);
    for (int gy = gy0; gy <= gy1; ++gy) {
      for (int gx = gx0; gx <= gx1; ++gx) {
        dist16[gy * gw + gx] = 0;
      }
    }
  };

  // 1. Scan actual 1-bit framebuffer ink pixels (catches every glyph, ruby mark, punctuation, rule, status bar).
  int minInkY = H;
  int maxInkY = 0;
  for (int y = 0; y < H; ++y) {
    const int gy = y / kCellPx;
    for (int x = 0; x < W; ++x) {
      if (readLogicalInkBit(renderer, fb, x, y)) {
        dist16[gy * gw + (x / kCellPx)] = 0;
        if (y < outField.usableBottom) {
          minInkY = std::min(minInkY, y);
          maxInkY = std::max(maxInkY, y);
        }
      }
    }
  }

  // Mark status bar zone solid so ornaments stay cleanly above the bottom margin/status bar.
  markRectOccupied(0, outField.usableBottom, W - 1, H - 1);

  // 2. Mark structural Page elements (solidifying word spaces on lines, paragraph leading, images, boxes).
  struct LineBox {
    int x0, y0, x1, y1;
  };
  std::vector<LineBox> lineBoxes;
  lineBoxes.reserve(48);

  const int baseLineH = std::max(16, renderer.getLineHeight(ctx.fontId));

  for (const auto& el : page.elements) {
    const PageElementTag tag = el->getTag();
    if (tag == TAG_PageImage) {
      const auto& img = static_cast<const PageImage&>(*el);
      const int ix = ctx.marginLeft + img.xPos;
      const int iy = ctx.marginTop + img.yPos;
      const int iw = img.getImageBlock().getWidth();
      const int ih = img.getImageBlock().getHeight();
      markRectOccupied(ix - 4, iy - 4, ix + iw + 4, iy + ih + 4);
      minInkY = std::min(minInkY, iy);
      maxInkY = std::max(maxInkY, iy + ih);
    } else if (tag == TAG_PageHorizontalRule) {
      const int ry = ctx.marginTop + el->yPos;
      markRectOccupied(ctx.marginLeft + el->xPos - 2, ry - 2, W - ctx.marginRight, ry + 4);
    } else if (tag == TAG_PageLine) {
      const auto& line = static_cast<const PageLine&>(*el);
      const TextBlock* block = line.getBlock();
      if (!block || !block->valid() || block->wordCount() == 0) continue;

      const int lineFont = TextBlock::renderFontId(renderer, block->blockFontId(ctx.fontId), ctx.fontId);
      const int lh = std::max(baseLineH, renderer.getLineHeight(lineFont));
      const int ly0 = std::max(0, ctx.marginTop + line.yPos - 2);
      const int ly1 = std::min(outField.usableBottom - 1, ctx.marginTop + line.yPos + lh + 4);

      // Measure horizontal ink span along this line band so ragged verse right-edges remain exact!
      int lx0 = W;
      int lx1 = -1;
      const int gy0 = ly0 / kCellPx;
      const int gy1 = ly1 / kCellPx;
      for (int gy = gy0; gy <= gy1; ++gy) {
        for (int gx = 0; gx < gw; ++gx) {
          if (dist16[gy * gw + gx] == 0) {
            lx0 = std::min(lx0, gx * kCellPx);
            lx1 = std::max(lx1, (gx + 1) * kCellPx - 1);
          }
        }
      }
      if (lx1 >= lx0) {
        markRectOccupied(lx0, ly0, lx1, ly1);
        lineBoxes.push_back({lx0, ly0, lx1, ly1});
        minInkY = std::min(minInkY, ly0);
        maxInkY = std::max(maxInkY, ly1);
      }
    }
  }

  // Bridge normal inter-line paragraph leading (<= 1.65 * lineHeight), while keeping true scene breaks open!
  std::sort(lineBoxes.begin(), lineBoxes.end(), [](const LineBox& a, const LineBox& b) { return a.y0 < b.y0; });
  const int maxParagraphLeadPx = std::max(26, static_cast<int>(baseLineH * 1.55f));
  for (size_t i = 0; i + 1 < lineBoxes.size(); ++i) {
    const auto& a = lineBoxes[i];
    const auto& b = lineBoxes[i + 1];
    const int gap = b.y0 - a.y1;
    if (gap >= 0 && gap <= maxParagraphLeadPx) {
      const int bx0 = std::max(a.x0, b.x0);
      const int bx1 = std::min(a.x1, b.x1);
      if (bx1 > bx0) {
        markRectOccupied(bx0, a.y1, bx1, b.y0);
      }
    }
  }

  outContentTop = (minInkY <= maxInkY) ? minInkY : ctx.marginTop;
  outContentBottom = (minInkY <= maxInkY) ? maxInkY : ctx.marginTop;

  // 3. Two-pass Rosenfeld-Pfaltz Chamfer (12, 17) distance transform (1 unit = 1/3 pixel; cell = 4 px = 12 units).
  constexpr uint16_t kOrth = 12;
  constexpr uint16_t kDiag = 17;

  for (int y = 0; y < gh; ++y) {
    for (int x = 0; x < gw; ++x) {
      uint16_t d = dist16[y * gw + x];
      if (d == 0) continue;
      if (x > 0) d = std::min<uint16_t>(d, dist16[y * gw + (x - 1)] + kOrth);
      if (y > 0) {
        d = std::min<uint16_t>(d, dist16[(y - 1) * gw + x] + kOrth);
        if (x > 0) d = std::min<uint16_t>(d, dist16[(y - 1) * gw + (x - 1)] + kDiag);
        if (x + 1 < gw) d = std::min<uint16_t>(d, dist16[(y - 1) * gw + (x + 1)] + kDiag);
      }
      dist16[y * gw + x] = d;
    }
  }

  for (int y = gh - 1; y >= 0; --y) {
    for (int x = gw - 1; x >= 0; --x) {
      uint16_t d = dist16[y * gw + x];
      if (x + 1 < gw) d = std::min<uint16_t>(d, dist16[y * gw + (x + 1)] + kOrth);
      if (y + 1 < gh) {
        d = std::min<uint16_t>(d, dist16[(y + 1) * gw + x] + kOrth);
        if (x + 1 < gw) d = std::min<uint16_t>(d, dist16[(y + 1) * gw + (x + 1)] + kDiag);
        if (x > 0) d = std::min<uint16_t>(d, dist16[(y + 1) * gw + (x - 1)] + kDiag);
      }
      dist16[y * gw + x] = d;
      const uint32_t px = (d + 1) / 3;
      outField.inkDistPx[y * gw + x] = static_cast<uint8_t>(std::min<uint32_t>(255u, px));
    }
  }

  return true;
}

std::vector<DeadRegion> extractDeadRegions(const ClearanceField& field, int contentTop, int contentBottom,
                                           std::vector<int16_t>& cellCompOut) {
  const int gw = field.gw;
  const int gh = field.gh;
  cellCompOut.assign(static_cast<size_t>(gw) * gh, -1);

  std::vector<DeadRegion> regions;
  std::vector<int> queue;
  queue.reserve(1024);

  int nextCompId = 0;

  for (int sy = 0; sy < gh; ++sy) {
    for (int sx = 0; sx < gw; ++sx) {
      const int idx0 = sy * gw + sx;
      if (cellCompOut[idx0] != -1) continue;
      const float cx0 = (sx + 0.5f) * kCellPx;
      const float cy0 = (sy + 0.5f) * kCellPx;
      if (field.clearanceAt(cx0, cy0) <= 3.0f) {
        cellCompOut[idx0] = -2;
        continue;
      }

      const int compId = nextCompId++;
      queue.clear();
      queue.push_back(idx0);
      cellCompOut[idx0] = static_cast<int16_t>(compId);

      int minGX = sx, maxGX = sx, minGY = sy, maxGY = sy;
      int countCells = 0;
      float bestScore = -1.0f;
      float bestClear = -1.0f;
      float bestX = cx0, bestY = cy0;

      size_t head = 0;
      while (head < queue.size()) {
        const int cur = queue[head++];
        const int gx = cur % gw;
        const int gy = cur / gw;
        const float px = (gx + 0.5f) * kCellPx;
        const float py = (gy + 0.5f) * kCellPx;
        const float c = field.clearanceAt(px, py);
        // Subtle horizontal/vertical centrality tie-breaker so flat horizontal bands center their primary pole!
        const float centerBias =
            -0.025f * std::fabs(px - field.screenW * 0.5f) + 0.04f * std::min(px, field.screenW - px);
        const float score = c + centerBias * 0.12f;

        minGX = std::min(minGX, gx);
        maxGX = std::max(maxGX, gx);
        minGY = std::min(minGY, gy);
        maxGY = std::max(maxGY, gy);
        ++countCells;

        if (score > bestScore && c >= bestClear - 2.5f) {
          bestScore = score;
          bestClear = std::max(bestClear, c);
          bestX = px;
          bestY = py;
        } else if (c > bestClear + 2.5f) {
          bestClear = c;
          bestScore = score;
          bestX = px;
          bestY = py;
        }

        constexpr int kDx[4] = {1, -1, 0, 0};
        constexpr int kDy[4] = {0, 0, 1, -1};
        for (int dir = 0; dir < 4; ++dir) {
          const int nx = gx + kDx[dir];
          const int ny = gy + kDy[dir];
          if (nx < 0 || nx >= gw || ny < 0 || ny >= gh) continue;
          const int nIdx = ny * gw + nx;
          if (cellCompOut[nIdx] != -1) continue;
          if (field.clearanceAt((nx + 0.5f) * kCellPx, (ny + 0.5f) * kCellPx) > 3.0f) {
            cellCompOut[nIdx] = static_cast<int16_t>(compId);
            queue.push_back(nIdx);
          } else {
            cellCompOut[nIdx] = -2;
          }
        }
      }

      const int areaPx = countCells * kCellPx * kCellPx;
      const int rx0 = minGX * kCellPx;
      const int ry0 = minGY * kCellPx;
      const int rx1 = std::min(field.screenW, (maxGX + 1) * kCellPx);
      const int ry1 = std::min(field.usableBottom, (maxGY + 1) * kCellPx);
      const int rw = rx1 - rx0;
      const int rh = ry1 - ry0;

      // Check qualification: either a major expanse or a horizontal scene-break corridor.
      const bool isMajorExpanse = (bestClear >= 24.0f && areaPx >= 8500 && rw >= 68 && rh >= 56);
      const bool isSceneBreak = (!isMajorExpanse && bestClear >= 10.5f && rw >= field.screenW * 0.42f && rh >= 22 &&
                                 rh <= 96 && ry0 > contentTop + 20 && ry1 < contentBottom - 20);

      if (!isMajorExpanse && !isSceneBreak) continue;

      DeadRegion reg;
      reg.componentId = compId;
      reg.x0 = rx0;
      reg.y0 = ry0;
      reg.x1 = rx1;
      reg.y1 = ry1;
      reg.areaPx = areaPx;

      if (isSceneBreak) {
        reg.kind = RegionKind::SceneBreakCorridor;
      } else if (ry1 >= field.usableBottom - 26 && bestY >= contentBottom - 16 && rw >= field.screenW * 0.48f) {
        reg.kind = RegionKind::TailpieceExpanse;
      } else if (ry0 <= 36 && bestY <= contentTop + 16 && rw >= field.screenW * 0.48f) {
        reg.kind = RegionKind::CrownExpanse;
      } else if (rh > rw * 1.15f) {
        reg.kind = RegionKind::MarginalBay;
      } else {
        reg.kind = (bestY > field.usableBottom * 0.5f) ? RegionKind::TailpieceExpanse : RegionKind::CrownExpanse;
      }

      // Extract up to 7 greedy inscribed-circle poles inside this component.
      reg.poles.push_back({bestX, bestY, bestClear});
      for (int p = 1; p < 7; ++p) {
        float candClear = -1.0f;
        float candX = 0.0f, candY = 0.0f;
        for (int gy = minGY; gy <= maxGY; ++gy) {
          for (int gx = minGX; gx <= maxGX; ++gx) {
            if (cellCompOut[gy * gw + gx] != compId) continue;
            const float px = (gx + 0.5f) * kCellPx;
            const float py = (gy + 0.5f) * kCellPx;
            const float c = field.clearanceAt(px, py);
            if (c < 14.0f || c <= candClear) continue;
            bool tooClose = false;
            for (const auto& prev : reg.poles) {
              const float minSep = std::max(44.0f, (prev.clearance + c) * 0.72f);
              const float dx = px - prev.x;
              const float dy = py - prev.y;
              if (dx * dx + dy * dy < minSep * minSep) {
                tooClose = true;
                break;
              }
            }
            if (!tooClose) {
              candClear = c;
              candX = px;
              candY = py;
            }
          }
        }
        if (candClear < 14.0f) break;
        reg.poles.push_back({candX, candY, candClear});
      }

      regions.push_back(std::move(reg));
    }
  }

  std::sort(regions.begin(), regions.end(),
            [](const DeadRegion& a, const DeadRegion& b) { return a.areaPx > b.areaPx; });
  if (regions.size() > 4) regions.resize(4);
  return regions;
}

// ============================================================================
// Phase 2: Vector Scene Graph & Strip-Streamed 1-Bit Atkinson/Knockout Painter
// ============================================================================

enum PixelMode : uint8_t {
  PM_BG = 0,           // Untouched background (paper white in light mode, night black in nocturne)
  PM_ATKINSON = 1,     // 1-bit Atkinson error-diffused physical brightness (0 = black, 255 = white)
  PM_FORCE_BLACK = 2,  // Crisp solid physical black (ink line in light mode, separation moat / pupil in nocturne)
  PM_FORCE_WHITE =
      3,  // Crisp solid physical white (paper knockout in light mode, bright chalk/silver line in nocturne)
};

enum class PetalShape : uint8_t {
  WildRoseNotched = 0,
  PeonyRuffled = 1,
  CosmosFluted = 2,
  PlumRound = 3,
  SakuraCleft = 4,
  BellCorolla = 5,
  TrumpetCorolla = 6,
  DaisyOblong = 7,
};

enum class LeafStyle : uint8_t {
  WoodcutHalfShade = 0,
  SolidInkBamboo = 1,
  InkWashBamboo = 2,
  HeartCordate = 3,
  ScallopedOak = 4,
};

enum class DiscKind : uint8_t {
  StamenCrown = 0,
  PlumCalyxPins = 1,
  Moon3D = 2,
  MorrisRosette = 3,
  StippledBud = 4,
  SeigaihaScale = 5,
  FacetedCompassStar = 6,
  SeedheadDome = 7,
  CelestialStar4 = 8,
  RingedStar = 9,
  RingedPlanet = 10,
};

enum class CreatureKind : uint8_t {
  Hummingbird = 0,
  TreeFrog = 1,
};

struct PrimBBox {
  int16_t x0, y0, x1, y1;
};

enum class PrimType : uint8_t {
  Stroke = 0,
  Petal = 1,
  Leaf = 2,
  Disc = 3,
  Wash = 4,
  Creature = 5,
};

struct ScenePrim {
  PrimType type = PrimType::Stroke;
  uint8_t zLayer = 2;
  PrimBBox box{0, 0, 0, 0};

  // Shared geometric parameters (compact union-free POD for MCU safety).
  float x0 = 0, y0 = 0;
  float x1 = 0, y1 = 0;
  float r0 = 1.0f, r1 = 1.0f;
  float angle = 0.0f;
  float length = 10.0f;
  float width = 6.0f;
  float curvature = 0.0f;
  uint8_t subKind = 0;
  uint8_t lumA = 90;
  uint8_t lumB = 250;
  uint8_t flags = 0;  // bit0: white knockout, bit1: serrated / drybrush, bit2: dotted
  uint32_t seed = 0;
};

struct SceneGraph {
  std::vector<ScenePrim> prims;

  void reserve(size_t n) { prims.reserve(n); }

  void addStroke(uint8_t z, float ax, float ay, float bx, float by, float ra, float rb, bool whiteHalo = false,
                 bool dryBrush = false, bool dotted = false, uint32_t seed = 0) {
    const float pad = std::max(ra, rb) + (whiteHalo ? 3.0f : 2.0f);
    ScenePrim p;
    p.type = PrimType::Stroke;
    p.zLayer = z;
    p.x0 = ax;
    p.y0 = ay;
    p.x1 = bx;
    p.y1 = by;
    p.r0 = ra;
    p.r1 = rb;
    p.flags = (whiteHalo ? 1u : 0u) | (dryBrush ? 2u : 0u) | (dotted ? 4u : 0u);
    p.seed = seed;
    p.box = {static_cast<int16_t>(std::floor(std::min(ax, bx) - pad)),
             static_cast<int16_t>(std::floor(std::min(ay, by) - pad)),
             static_cast<int16_t>(std::ceil(std::max(ax, bx) + pad)),
             static_cast<int16_t>(std::ceil(std::max(ay, by) + pad))};
    prims.push_back(p);
  }

  void addPetal(uint8_t z, float cx, float cy, float angle, float len, float wid, PetalShape shape, uint8_t throatLum,
                uint8_t tipLum, uint32_t seed = 0) {
    const float reach = len + wid * 0.7f + 5.0f;
    ScenePrim p;
    p.type = PrimType::Petal;
    p.zLayer = z;
    p.x0 = cx;
    p.y0 = cy;
    p.angle = angle;
    p.length = len;
    p.width = wid;
    p.subKind = static_cast<uint8_t>(shape);
    p.lumA = throatLum;
    p.lumB = tipLum;
    p.flags = 1u;  // Always crisp white paper knockout rim
    p.seed = seed;
    p.box = {static_cast<int16_t>(std::floor(cx - reach)), static_cast<int16_t>(std::floor(cy - reach)),
             static_cast<int16_t>(std::ceil(cx + reach)), static_cast<int16_t>(std::ceil(cy + reach))};
    prims.push_back(p);
  }

  void addLeaf(uint8_t z, float bx, float by, float angle, float len, float wid, float bend, LeafStyle style,
               bool serrated, uint32_t seed = 0) {
    const float reach = len + std::fabs(bend) * len + wid + 5.0f;
    ScenePrim p;
    p.type = PrimType::Leaf;
    p.zLayer = z;
    p.x0 = bx;
    p.y0 = by;
    p.angle = angle;
    p.length = len;
    p.width = wid;
    p.curvature = bend;
    p.subKind = static_cast<uint8_t>(style);
    p.flags = 1u | (serrated ? 2u : 0u);
    p.seed = seed;
    p.box = {static_cast<int16_t>(std::floor(bx - reach)), static_cast<int16_t>(std::floor(by - reach)),
             static_cast<int16_t>(std::ceil(bx + reach)), static_cast<int16_t>(std::ceil(by + reach))};
    prims.push_back(p);
  }

  void addDisc(uint8_t z, float cx, float cy, float radius, DiscKind kind, float phaseAngle = 0.0f, uint8_t lumA = 80,
               uint8_t lumB = 248, uint32_t seed = 0) {
    const float reach = radius * (kind == DiscKind::RingedPlanet ? 2.35f : 1.55f) + 5.0f;
    ScenePrim p;
    p.type = PrimType::Disc;
    p.zLayer = z;
    p.x0 = cx;
    p.y0 = cy;
    p.r0 = radius;
    p.angle = phaseAngle;
    p.subKind = static_cast<uint8_t>(kind);
    p.lumA = lumA;
    p.lumB = lumB;
    p.seed = seed;
    p.box = {static_cast<int16_t>(std::floor(cx - reach)), static_cast<int16_t>(std::floor(cy - reach)),
             static_cast<int16_t>(std::ceil(cx + reach)), static_cast<int16_t>(std::ceil(cy + reach))};
    prims.push_back(p);
  }

  void addCreature(uint8_t z, CreatureKind kind, float cx, float cy, float scale, float angle, float facing,
                   uint32_t seed = 0) {
    const float reach = scale * (kind == CreatureKind::Hummingbird ? 1.48f : 1.15f) + 6.0f;
    ScenePrim p;
    p.type = PrimType::Creature;
    p.zLayer = z;
    p.x0 = cx;
    p.y0 = cy;
    p.r0 = scale;
    p.angle = angle;
    p.curvature = facing;
    p.subKind = static_cast<uint8_t>(kind);
    p.seed = seed;
    p.box = {static_cast<int16_t>(std::floor(cx - reach)), static_cast<int16_t>(std::floor(cy - reach)),
             static_cast<int16_t>(std::ceil(cx + reach)), static_cast<int16_t>(std::ceil(cy + reach))};
    prims.push_back(p);
  }

  void addWash(int rx0, int ry0, int rx1, int ry1, uint8_t washStyle, uint8_t minLum, uint32_t seed) {
    ScenePrim p;
    p.type = PrimType::Wash;
    p.zLayer = 0;
    p.subKind = washStyle;  // 0 = Shan-Shui mountain ridge/mist, 1 = Milky Way nebula band
    p.lumA = minLum;
    p.seed = seed;
    p.x0 = static_cast<float>(rx0);
    p.y0 = static_cast<float>(ry0);
    p.x1 = static_cast<float>(rx1);
    p.y1 = static_cast<float>(ry1);
    p.box = {static_cast<int16_t>(rx0), static_cast<int16_t>(ry0), static_cast<int16_t>(rx1),
             static_cast<int16_t>(ry1)};
    prims.push_back(p);
  }
};

// ============================================================================
// High-Craft Botanical & Geometric Motif Builders
// ============================================================================

// Ensures a circular motif (like a 5-petal blossom) has positive clearance around its ENTIRE perimeter
// so no single petal is ever clipped off by the sacred text halo!
bool fitSafeBloomRadius(const ClearanceField& field, float& ioX, float& ioY, float desiredR, float minR,
                        float& outSafeR) {
  float cx = ioX;
  float cy = ioY;
  for (int step = 0; step < 3; ++step) {
    float minPerim = field.clearanceAt(cx, cy) - desiredR;
    for (int k = 0; k < 8; ++k) {
      const float a = (kTwoPi * k) / 8.0f;
      const float c = field.clearanceAt(cx + std::cos(a) * (desiredR + 2.0f), cy + std::sin(a) * (desiredR + 2.0f));
      minPerim = std::min(minPerim, c);
    }
    if (minPerim >= 2.5f) {
      ioX = cx;
      ioY = cy;
      outSafeR = desiredR;
      return true;
    }
    float gx = 0.0f, gy = 0.0f;
    field.gradientAt(cx, cy, gx, gy);
    cx += gx * 3.5f;
    cy += gy * 3.5f;
    desiredR *= 0.88f;
    if (desiredR < minR) break;
  }
  const float cCenter = field.clearanceAt(cx, cy);
  float safeR = std::min(desiredR, cCenter - 3.5f);
  for (int k = 0; k < 8; ++k) {
    const float a = (kTwoPi * k) / 8.0f;
    if (field.clearanceAt(cx + std::cos(a) * (safeR + 2.0f), cy + std::sin(a) * (safeR + 2.0f)) < 2.2f) {
      safeR *= 0.82f;
    }
  }
  if (safeR < minR) return false;
  ioX = cx;
  ioY = cy;
  outSafeR = safeR;
  return true;
}

void emitCurvedPolyline(SceneGraph& sg, uint8_t z, const std::vector<std::pair<float, float>>& pts, float rStart,
                        float rEnd, bool whiteHalo, bool dryBrush = false, uint32_t seed = 0) {
  if (pts.size() < 2) return;
  const size_t n = pts.size();
  for (size_t i = 0; i + 1 < n; ++i) {
    const float t0 = static_cast<float>(i) / static_cast<float>(n - 1);
    const float t1 = static_cast<float>(i + 1) / static_cast<float>(n - 1);
    sg.addStroke(z, pts[i].first, pts[i].second, pts[i + 1].first, pts[i + 1].second, lerpf(rStart, rEnd, t0),
                 lerpf(rStart, rEnd, t1), whiteHalo, dryBrush, false, seed + static_cast<uint32_t>(i));
  }
}

void emitSpiralTendril(SceneGraph& sg, const ClearanceField& field, float sx, float sy, float startAngle,
                       float initRadius, float turnSign, uint8_t z = 2) {
  std::vector<std::pair<float, float>> pts;
  pts.reserve(28);
  float x = sx;
  float y = sy;
  float theta = startAngle;
  const int steps = 24;
  pts.push_back({x, y});
  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    const float stepLen = initRadius * 0.22f * (1.0f - 0.55f * t);
    theta += turnSign * (0.16f + 0.34f * t);
    const float nx = x + std::cos(theta) * stepLen;
    const float ny = y + std::sin(theta) * stepLen;
    if (field.clearanceAt(nx, ny) < 3.0f) break;
    x = nx;
    y = ny;
    pts.push_back({x, y});
  }
  emitCurvedPolyline(sg, z, pts, 0.85f, 0.48f, false);
}

// Compound bilateral pinnate fern frond with opposite leaflet pairs on BOTH sides of a curved rachis!
void emitFernFrond(SceneGraph& sg, const ClearanceField& field, float bx, float by, float startAngle, float totalLen,
                   float bendSign, Rng& rng) {
  std::vector<std::pair<float, float>> rachis;
  const int steps = 9;
  float x = bx, y = by, ang = startAngle;
  rachis.push_back({x, y});
  for (int i = 1; i <= steps; ++i) {
    const float stepL = totalLen / static_cast<float>(steps);
    ang += bendSign * 0.065f;
    const float nx = x + std::cos(ang) * stepL;
    const float ny = y + std::sin(ang) * stepL;
    if (field.clearanceAt(nx, ny) < 4.5f) break;
    x = nx;
    y = ny;
    rachis.push_back({x, y});

    if (i >= 2 && i <= steps - 1) {
      const float t = static_cast<float>(i) / static_cast<float>(steps);
      const float pinnaLen = clampf(totalLen * 0.32f * std::sin(std::pow(t, 0.65f) * kPi), 5.5f, 13.5f);
      for (int s = -1; s <= 1; s += 2) {
        const float pa = ang + s * 0.96f;
        if (field.clearanceAt(x + std::cos(pa) * pinnaLen, y + std::sin(pa) * pinnaLen) >= 3.0f) {
          const bool useCrisp = (pinnaLen < 8.5f);
          sg.addLeaf(3, x, y, pa, pinnaLen, pinnaLen * (useCrisp ? 0.28f : 0.38f), s * 0.10f,
                     useCrisp ? LeafStyle::SolidInkBamboo : LeafStyle::WoodcutHalfShade, false, rng.nextU32());
        }
      }
    }
  }
  emitCurvedPolyline(sg, 2, rachis, 0.92f, 0.45f, false, false, rng.nextU32());
  if (rachis.size() >= 5) {
    emitSpiralTendril(sg, field, rachis.back().first, rachis.back().second, ang, 7.5f, bendSign, 2);
  }
}

// Botanical berry cluster (3-4 round glossy berries on short forked pedicels).
void emitBerryCluster(SceneGraph& sg, const ClearanceField& field, float bx, float by, float baseAng, float scale,
                      Rng& rng) {
  const int nBerries = 3;
  for (int k = -1; k <= 1; ++k) {
    const float a = baseAng + k * 0.46f + rng.range(-0.06f, 0.06f);
    const float stemL = scale * (k == 0 ? 1.05f : 0.85f);
    const float cx = bx + std::cos(a) * stemL;
    const float cy = by + std::sin(a) * stemL;
    if (field.clearanceAt(cx, cy) < 5.5f) continue;
    sg.addStroke(2, bx, by, cx, cy, 0.72f, 0.55f, false);
    const float br = clampf(scale * 0.34f, 3.2f, 5.2f);
    sg.addDisc(6, cx, cy, br, DiscKind::StippledBud, a, 72, 250, rng.nextU32());
    // Tiny blossom-end crown tick at tip of berry.
    sg.addStroke(7, cx + std::cos(a) * (br - 0.5f), cy + std::sin(a) * (br - 0.5f), cx + std::cos(a) * (br + 1.4f),
                 cy + std::sin(a) * (br + 1.4f), 0.55f, 0.55f, false);
  }
}

// Pendulous side-profile Bellflower / Foxglove / Lily-of-the-Valley dangling from an arched stem!
bool emitBellFlower(SceneGraph& sg, const ClearanceField& field, float stemX, float stemY, float outAng, float bellLen,
                    Rng& rng) {
  // Arch the slender pedicel outward and gently downward so the bell hangs naturally under gravity!
  const float midAng = std::atan2(std::sin(outAng) * 0.55f - 0.35f, std::cos(outAng));
  const float capX = stemX + std::cos(midAng) * (bellLen * 0.52f);
  const float capY = stemY + std::sin(midAng) * (bellLen * 0.52f) + 2.5f;
  const float hangAng = kPi * 0.5f + (std::cos(outAng) >= 0.0f ? -0.28f : 0.28f) + rng.range(-0.10f, 0.10f);
  const float mouthX = capX + std::cos(hangAng) * bellLen;
  const float mouthY = capY + std::sin(hangAng) * bellLen;

  if (field.clearanceAt(capX, capY) < bellLen * 0.45f || field.clearanceAt(mouthX, mouthY) < bellLen * 0.48f) {
    return false;
  }

  std::vector<std::pair<float, float>> pedicel = {
      {stemX, stemY}, {(stemX + capX) * 0.5f, std::min(stemY, capY) - 2.5f}, {capX, capY}};
  emitCurvedPolyline(sg, 2, pedicel, 0.85f, 0.65f, false);

  // Two delicate protruding stamen clappers hanging just past the scalloped bell mouth!
  for (int s = -1; s <= 1; s += 2) {
    const float sa = hangAng + s * 0.14f;
    const float sx = capX + std::cos(sa) * (bellLen * 1.14f);
    const float sy = capY + std::sin(sa) * (bellLen * 1.14f);
    sg.addStroke(4, capX + std::cos(sa) * (bellLen * 0.78f), capY + std::sin(sa) * (bellLen * 0.78f), sx, sy, 0.52f,
                 0.52f, false);
    sg.addDisc(5, sx, sy, 1.35f, DiscKind::StippledBud, 0.0f, 0, 10);
  }

  // Flared bell corolla + crisp ink calyx cap at top!
  sg.addPetal(5, capX, capY, hangAng, bellLen, bellLen * 0.86f, PetalShape::BellCorolla, 68, 242, rng.nextU32());
  sg.addLeaf(6, capX, capY, hangAng + 0.52f, bellLen * 0.36f, bellLen * 0.14f, -0.15f, LeafStyle::SolidInkBamboo, false,
             rng.nextU32());
  sg.addLeaf(6, capX, capY, hangAng - 0.52f, bellLen * 0.36f, bellLen * 0.14f, 0.15f, LeafStyle::SolidInkBamboo, false,
             rng.nextU32());
  return true;
}

// Side-profile Nectar Trumpet / Honeysuckle / Morning Glory (ideal partner for a hovering hummingbird!).
bool emitTrumpetFlower(SceneGraph& sg, const ClearanceField& field, float baseX, float baseY, float aimAng, float len,
                       float& outMouthX, float& outMouthY, Rng& rng) {
  const float mx = baseX + std::cos(aimAng) * len;
  const float my = baseY + std::sin(aimAng) * len;
  if (field.clearanceAt(mx, my) < len * 0.45f || field.clearanceAt((baseX + mx) * 0.5f, (baseY + my) * 0.5f) < 7.0f) {
    return false;
  }
  outMouthX = mx;
  outMouthY = my;

  // Protruding curved nectar stamens extending slightly past the trumpet rim!
  for (int s = -1; s <= 1; s += 2) {
    const float sa = aimAng + s * 0.11f;
    const float ax = baseX + std::cos(sa) * (len * 1.15f);
    const float ay = baseY + std::sin(sa) * (len * 1.15f);
    if (field.clearanceAt(ax, ay) >= 4.0f) {
      sg.addStroke(6, baseX + std::cos(sa) * (len * 0.82f), baseY + std::sin(sa) * (len * 0.82f), ax, ay, 0.52f, 0.52f,
                   false);
      sg.addDisc(7, ax, ay, 1.4f, DiscKind::StippledBud, 0.0f, 0, 10);
    }
  }

  sg.addPetal(5, baseX, baseY, aimAng, len, len * 0.88f, PetalShape::TrumpetCorolla, 58, 245, rng.nextU32());
  // Embracing sepal cup at base of floral tube.
  sg.addLeaf(6, baseX, baseY, aimAng + 0.38f, len * 0.32f, len * 0.12f, -0.10f, LeafStyle::SolidInkBamboo, false,
             rng.nextU32());
  sg.addLeaf(6, baseX, baseY, aimAng - 0.38f, len * 0.32f, len * 0.12f, 0.10f, LeafStyle::SolidInkBamboo, false,
             rng.nextU32());
  return true;
}

void emitBotanicalFlower(SceneGraph& sg, float cx, float cy, float radius, int species, float orientAng, Rng& rng) {
  const float baseRot = rng.range(0.0f, kTwoPi);
  const int sp = species % 5;
  if (sp == 0) {
    // Species 0: Plump Round-Petaled Wild Dog-Rose / Eglantine (broad soft rounded petals, not spiky!)
    const int nOuter = 5;
    for (int i = 0; i < nOuter; ++i) {
      const float a = baseRot + (kTwoPi * i) / nOuter + rng.range(-0.04f, 0.04f);
      sg.addPetal((i % 2 == 0) ? 4 : 5, cx, cy, a, radius * rng.range(0.95f, 1.02f), radius * 0.82f,
                  PetalShape::PlumRound, 82, 248, rng.nextU32());
    }
    sg.addDisc(7, cx, cy, radius * 0.34f, DiscKind::StamenCrown, baseRot, 55, 250, rng.nextU32());
  } else if (sp == 1) {
    // Species 1: Botanical Sunflower / Coneflower Daisy (large cross-hatched seedhead dome + 10 broad rounded rays)
    const int nRay = 10;
    for (int i = 0; i < nRay; ++i) {
      const float a = baseRot + (kTwoPi * i) / nRay + rng.range(-0.03f, 0.03f);
      sg.addPetal((i % 2 == 0) ? 4 : 5, cx, cy, a, radius * rng.range(0.95f, 1.03f), radius * 0.42f,
                  PetalShape::DaisyOblong, 108, 252, rng.nextU32());
    }
    sg.addDisc(7, cx, cy, radius * 0.41f, DiscKind::SeedheadDome, baseRot, 48, 225, rng.nextU32());
  } else if (sp == 2) {
    // Species 2: Upright 3-Petal Cupped Goblet Tulip / Magnolia in side profile!
    const float upAng = (std::sin(orientAng) > 0.35f) ? (-kPi * 0.5f + rng.range(-0.35f, 0.35f)) : orientAng;
    const float bx = cx - std::cos(upAng) * (radius * 0.45f);
    const float by = cy - std::sin(upAng) * (radius * 0.45f);
    // Two curving outer side petals + central cupped front petal!
    sg.addPetal(4, bx, by, upAng - 0.36f, radius * 1.18f, radius * 0.62f, PetalShape::PeonyRuffled, 72, 244,
                rng.nextU32());
    sg.addPetal(4, bx, by, upAng + 0.36f, radius * 1.18f, radius * 0.62f, PetalShape::PeonyRuffled, 72, 244,
                rng.nextU32());
    sg.addPetal(5, bx, by, upAng, radius * 1.24f, radius * 0.72f, PetalShape::PlumRound, 64, 248, rng.nextU32());
    sg.addLeaf(6, bx, by, upAng + 0.62f, radius * 0.52f, radius * 0.16f, -0.14f, LeafStyle::SolidInkBamboo, false,
               rng.nextU32());
    sg.addLeaf(6, bx, by, upAng - 0.62f, radius * 0.52f, radius * 0.16f, 0.14f, LeafStyle::SolidInkBamboo, false,
               rng.nextU32());
  } else if (sp == 3) {
    // Species 3: Layered Ruffled Camellia / Peony (lush concentric rounded-ruffled rings)
    const int nBack = 6;
    for (int i = 0; i < nBack; ++i) {
      const float a = baseRot + (kTwoPi * i) / nBack;
      sg.addPetal(4, cx, cy, a, radius * 0.98f, radius * 0.68f, PetalShape::PeonyRuffled, 68, 244, rng.nextU32());
    }
    const int nMid = 5;
    for (int i = 0; i < nMid; ++i) {
      const float a = baseRot + 0.42f + (kTwoPi * i) / nMid;
      sg.addPetal(5, cx, cy, a, radius * 0.68f, radius * 0.54f, PetalShape::PlumRound, 56, 240, rng.nextU32());
    }
    sg.addDisc(7, cx, cy, radius * 0.23f, DiscKind::StamenCrown, baseRot, 48, 248, rng.nextU32());
  } else {
    // Species 4: Heart-Notched Tudor Rose
    const int nOuter = 5;
    for (int i = 0; i < nOuter; ++i) {
      const float a = baseRot + (kTwoPi * i) / nOuter;
      sg.addPetal(4, cx, cy, a, radius, radius * 0.74f, PetalShape::WildRoseNotched, 62, 244, rng.nextU32());
    }
    for (int i = 0; i < nOuter; ++i) {
      const float a = baseRot + (kTwoPi * (i + 0.5f)) / nOuter;
      sg.addPetal(5, cx, cy, a, radius * 0.64f, radius * 0.50f, PetalShape::WildRoseNotched, 48, 236, rng.nextU32());
    }
    sg.addDisc(7, cx, cy, radius * 0.26f, DiscKind::StamenCrown, baseRot, 55, 250, rng.nextU32());
  }
}

void emitRoseBud(SceneGraph& sg, float bx, float by, float angle, float size, Rng& rng) {
  const float ux = std::cos(angle);
  const float uy = std::sin(angle);
  sg.addPetal(5, bx, by, angle, size, size * 0.58f, PetalShape::PlumRound, 85, 246, rng.nextU32());
  // Two embracing outer sepals in crisp ink.
  sg.addLeaf(6, bx - ux * 1.5f, by - uy * 1.5f, angle + 0.42f, size * 0.72f, size * 0.18f, -0.18f,
             LeafStyle::SolidInkBamboo, false, rng.nextU32());
  sg.addLeaf(6, bx - ux * 1.5f, by - uy * 1.5f, angle - 0.42f, size * 0.72f, size * 0.18f, 0.18f,
             LeafStyle::SolidInkBamboo, false, rng.nextU32());
}

// Full 5-petal classical plum blossom — always renders all 5 complete round petals!
void emitPlumBlossom(SceneGraph& sg, float cx, float cy, float radius, Rng& rng) {
  const float rot = rng.range(0.0f, kTwoPi);
  for (int i = 0; i < 5; ++i) {
    const float a = rot + (kTwoPi * i) / 5.0f + rng.range(-0.03f, 0.03f);
    sg.addPetal((i % 2 == 0) ? 5 : 6, cx, cy, a, radius, radius * 0.74f, PetalShape::PlumRound, 148, 254,
                rng.nextU32());
  }
  sg.addDisc(7, cx, cy, radius * 0.36f, DiscKind::PlumCalyxPins, rot, 40, 255, rng.nextU32());
}

// Physically attached plum pearl bud: draws a crisp ink pedicel stem + 3-prong ink calyx cup + round pearl bud
// so zero floating dots ever appear around plum blossoms!
void emitAttachedPlumBud(SceneGraph& sg, const ClearanceField& field, float branchX, float branchY, float budAng,
                         float stemLen, float budR, Rng& rng) {
  const float bx = branchX + std::cos(budAng) * stemLen;
  const float by = branchY + std::sin(budAng) * stemLen;
  if (field.clearanceAt(bx, by) < budR + 3.0f) return;

  // Crisp ink twig pedicel connecting branch directly into the bud calyx!
  sg.addStroke(3, branchX, branchY, bx, by, 0.88f, 0.70f, false);
  // Pearl bud seated at tip of pedicel.
  const float cx = bx + std::cos(budAng) * (budR * 0.72f);
  const float cy = by + std::sin(budAng) * (budR * 0.72f);
  if (field.clearanceAt(cx, cy) < budR + 2.5f) return;
  sg.addDisc(5, cx, cy, budR, DiscKind::StippledBud, budAng, 108, 252, rng.nextU32());
  // Crisp 2-sepal dark ink calyx cup embracing the base of the pearl bud!
  sg.addLeaf(6, bx, by, budAng + 0.55f, budR * 1.15f, budR * 0.42f, -0.22f, LeafStyle::SolidInkBamboo, false,
             rng.nextU32());
  sg.addLeaf(6, bx, by, budAng - 0.55f, budR * 1.15f, budR * 0.42f, 0.22f, LeafStyle::SolidInkBamboo, false,
             rng.nextU32());
}

void emitSakuraFlowerOrPetal(SceneGraph& sg, float cx, float cy, float radius, bool fullBloom, Rng& rng) {
  const float rot = rng.range(0.0f, kTwoPi);
  if (fullBloom) {
    for (int i = 0; i < 5; ++i) {
      const float a = rot + (kTwoPi * i) / 5.0f;
      sg.addPetal((i % 2 == 0) ? 40 : 41, cx, cy, a, radius, radius * 0.64f, PetalShape::SakuraCleft, 132, 254,
                  rng.nextU32());
    }
    sg.addDisc(42, cx, cy, radius * 0.30f, DiscKind::PlumCalyxPins, rot, 55, 255, rng.nextU32());
  } else {
    // Single tumbling notched sakura petal in the wind.
    sg.addPetal(41, cx, cy, rot, radius, radius * rng.range(0.54f, 0.72f), PetalShape::SakuraCleft, 140, 254,
                rng.nextU32());
  }
}

// ============================================================================
// Theme 1: Botanical Woodcut (Space-Colonization Vines, Roses, Cosmos & Leaves)
// ============================================================================

void buildBotanicalFloral(SceneGraph& sg, const ClearanceField& field, const DeadRegion& reg,
                          const std::vector<int16_t>& cellComp, Rng& rng) {
  if (reg.poles.empty()) return;
  const Pole& mainPole = reg.poles[0];

  if (reg.kind == RegionKind::SceneBreakCorridor) {
    const float cy = mainPole.y;
    const float span = std::min(170.0f, (reg.x1 - reg.x0) * 0.42f);
    // Start from left or right end of the corridor and grow organically across!
    const float dir = rng.chance(0.5f) ? 1.0f : -1.0f;
    const float sx = (reg.x0 + reg.x1) * 0.5f - dir * span;
    const float flowerR = clampf(mainPole.clearance * 0.72f, 8.5f, 14.5f);

    std::vector<std::pair<float, float>> stem;
    const int nPts = 22;
    int leafParity = 1;
    for (int i = 0; i <= nPts; ++i) {
      const float t = static_cast<float>(i) / nPts;
      const float px = sx + dir * t * (2.0f * span);
      const float py = cy + std::sin(t * kPi * 2.2f) * std::min(5.0f, mainPole.clearance * 0.28f);
      if (field.clearanceAt(px, py) < 3.0f) break;
      stem.push_back({px, py});
      if (i >= 2 && i <= nPts - 2 && i % 2 == 0) {
        const float side = static_cast<float>(leafParity);
        leafParity = -leafParity;
        const float leafAng = (dir > 0 ? 0.0f : kPi) + side * 0.72f;
        const float leafLen = clampf(flowerR * (1.15f - 0.30f * t), 8.5f, 15.5f);
        const LeafStyle ls = (i % 4 == 0) ? LeafStyle::HeartCordate : LeafStyle::WoodcutHalfShade;
        sg.addLeaf(3, px, py, leafAng, leafLen, leafLen * (ls == LeafStyle::HeartCordate ? 0.58f : 0.42f), side * 0.14f,
                   ls, ls == LeafStyle::WoodcutHalfShade, rng.nextU32());
      }
    }
    emitCurvedPolyline(sg, 2, stem, 1.55f, 0.58f, true, false, rng.nextU32());
    if (stem.size() >= 6) {
      const auto& tip = stem.back();
      emitRoseBud(sg, tip.first, tip.second, dir > 0 ? 0.0f : kPi, flowerR * 0.68f, rng);
    }
    float fx = (reg.x0 + reg.x1) * 0.5f - dir * span * 0.22f;
    float fy = cy;
    float safeR = flowerR;
    if (fitSafeBloomRadius(field, fx, fy, flowerR, 8.0f, safeR)) {
      emitBotanicalFlower(sg, fx, fy, safeR, rng.rangeInt(0, 2), -kPi * 0.5f, rng);
    }
    float bx = (reg.x0 + reg.x1) * 0.5f + dir * span * 0.35f;
    emitBellFlower(sg, field, bx, cy - 2.0f, dir > 0 ? 0.2f : (kPi - 0.2f), safeR * 0.85f, rng);
    return;
  }

  // 1. Choose a grounded plant root along the BOTTOM of the dead space (random bottom corner or bottom edge)!
  struct BottomCand {
    float x, y, c;
  };
  std::vector<BottomCand> bottomCands;
  const int gMinX = std::max(0, reg.x0 / kCellPx);
  const int gMaxX = std::min(field.gw - 1, reg.x1 / kCellPx);
  const int gMinY = std::max(0, reg.y0 / kCellPx);
  const int gMaxY = std::min(field.gh - 1, reg.y1 / kCellPx);
  const float bottomBandTop = static_cast<float>(reg.y1) - std::max(44.0f, (reg.y1 - reg.y0) * 0.28f);

  for (int gy = gMinY; gy <= gMaxY; ++gy) {
    const float py = (gy + 0.5f) * kCellPx;
    if (py < bottomBandTop) continue;
    for (int gx = gMinX; gx <= gMaxX; ++gx) {
      if (cellComp[gy * field.gw + gx] != reg.componentId) continue;
      const float px = (gx + 0.5f) * kCellPx;
      const float c = field.clearanceAt(px, py);
      if (c >= 11.5f) {
        bottomCands.push_back({px, py, c});
      }
    }
  }

  float rootX = mainPole.x;
  float rootY = mainPole.y;
  if (!bottomCands.empty()) {
    // 70% chance: sprout from a random bottom-left or bottom-right corner; 30% chance: random bottom spot!
    const int mode = rng.rangeInt(0, 9);
    float bestScore = -1e9f;
    for (const auto& bc : bottomCands) {
      const float uX = (bc.x - reg.x0) / std::max(1.0f, static_cast<float>(reg.x1 - reg.x0));
      const float uY = (bc.y - reg.y0) / std::max(1.0f, static_cast<float>(reg.y1 - reg.y0));
      float score = uY * 32.0f + std::min(bc.c, 24.0f) * 0.45f + rng.range(-4.5f, 4.5f);
      if (mode < 4) {
        // Bottom-left corner bias
        score += (1.0f - uX) * 38.0f;
      } else if (mode < 7) {
        // Bottom-right corner bias
        score += uX * 38.0f;
      } else {
        // Random grounded position along the bottom edge
        score += rng.range(0.0f, 18.0f);
      }
      if (score > bestScore) {
        bestScore = score;
        rootX = bc.x;
        rootY = bc.y;
      }
    }
  }

  // Initial upward-and-inward shoot momentum from the bottom root toward the open interior!
  float initDX = mainPole.x - rootX;
  float initDY = std::min(-18.0f, mainPole.y - rootY - 12.0f);
  const float initLen = std::max(1.0f, std::hypot(initDX, initDY));
  initDX /= initLen;
  initDY /= initLen;

  // Anchor the plant's base with a lush basal crown (2-3 arching basal leaves / fern frond at root)!
  const float rootBaseAng = std::atan2(initDY, initDX);
  for (int b = -1; b <= 1; ++b) {
    const float ba = rootBaseAng + b * 0.62f + rng.range(-0.10f, 0.10f);
    const float bLen = clampf(field.clearanceAt(rootX, rootY) * rng.range(0.85f, 1.15f), 14.0f, 28.0f);
    if (b != 0 && rng.chance(0.55f)) {
      emitFernFrond(sg, field, rootX, rootY, ba, bLen * 1.12f, (b > 0) ? 1.0f : -1.0f, rng);
    } else if (field.clearanceAt(rootX + std::cos(ba) * bLen, rootY + std::sin(ba) * bLen) >= 4.0f) {
      sg.addLeaf(3, rootX, rootY, ba, bLen, bLen * 0.48f, b * 0.16f, LeafStyle::ScallopedOak, false, rng.nextU32());
    }
  }

  // 2. Space-colonization vine skeleton growing organically UPWARD & ACROSS from (rootX, rootY)!
  struct VineNode {
    float x, y;
    int parent;
    int depth;
    float dirX, dirY;
  };

  const bool isTallCanvas = (reg.y1 - reg.y0) > 460;
  const size_t maxVineNodes = isTallCanvas ? 360 : 225;
  const int maxGrowIters = isTallCanvas ? 86 : 56;

  std::vector<VineNode> nodes;
  nodes.reserve(maxVineNodes + 16);
  nodes.push_back({rootX, rootY, -1, 0, initDX, initDY});

  struct Attractor {
    float x, y;
    bool active;
  };
  std::vector<Attractor> attractors;
  const int targetAttr = std::clamp(reg.areaPx / (isTallCanvas ? 540 : 820), 40, isTallCanvas ? 290 : 175);
  attractors.reserve(targetAttr + reg.poles.size());
  for (const auto& pole : reg.poles) {
    if (std::hypot(pole.x - rootX, pole.y - rootY) > 22.0f) {
      attractors.push_back({pole.x, pole.y, true});
    }
  }
  for (int attempt = 0; attempt < targetAttr * 8 && static_cast<int>(attractors.size()) < targetAttr; ++attempt) {
    const float ax = rng.range(static_cast<float>(reg.x0 + 8), static_cast<float>(reg.x1 - 8));
    const float ay = rng.range(static_cast<float>(reg.y0 + 8), static_cast<float>(reg.y1 - 8));
    const int gx = std::clamp(static_cast<int>(ax) / kCellPx, 0, field.gw - 1);
    const int gy = std::clamp(static_cast<int>(ay) / kCellPx, 0, field.gh - 1);
    if (cellComp[gy * field.gw + gx] != reg.componentId) continue;
    if (field.clearanceAt(ax, ay) < 10.0f) continue;

    bool tooClose = false;
    for (const auto& ex : attractors) {
      const float dx = ax - ex.x;
      const float dy = ay - ex.y;
      if (dx * dx + dy * dy < 23.0f * 23.0f) {
        tooClose = true;
        break;
      }
    }
    if (!tooClose) attractors.push_back({ax, ay, true});
  }

  constexpr float kStepLen = 14.5f;
  constexpr float kKillDist = 19.5f;
  const float kInfluenceDist = isTallCanvas ? 185.0f : 145.0f;

  std::vector<float> pullX;
  std::vector<float> pullY;
  std::vector<int> pullCount;

  for (int iter = 0; iter < maxGrowIters && nodes.size() < maxVineNodes; ++iter) {
    pullX.assign(nodes.size(), 0.0f);
    pullY.assign(nodes.size(), 0.0f);
    pullCount.assign(nodes.size(), 0);

    int activeRemaining = 0;
    for (auto& att : attractors) {
      if (!att.active) continue;
      int bestNode = -1;
      float bestD2 = kInfluenceDist * kInfluenceDist;
      for (size_t ni = 0; ni < nodes.size(); ++ni) {
        const float dx = att.x - nodes[ni].x;
        const float dy = att.y - nodes[ni].y;
        const float d2 = dx * dx + dy * dy;
        if (d2 < kKillDist * kKillDist) {
          att.active = false;
          bestNode = -1;
          break;
        }
        if (d2 < bestD2) {
          bestD2 = d2;
          bestNode = static_cast<int>(ni);
        }
      }
      if (!att.active) continue;
      ++activeRemaining;
      if (bestNode >= 0) {
        const float d = std::sqrt(bestD2);
        pullX[bestNode] += (att.x - nodes[bestNode].x) / d;
        pullY[bestNode] += (att.y - nodes[bestNode].y) / d;
        pullCount[bestNode]++;
      }
    }
    if (activeRemaining == 0) break;

    const size_t curCount = nodes.size();
    for (size_t ni = 0; ni < curCount && nodes.size() < maxVineNodes; ++ni) {
      if (pullCount[ni] == 0) continue;
      float dx = pullX[ni] / static_cast<float>(pullCount[ni]);
      float dy = pullY[ni] / static_cast<float>(pullCount[ni]);
      float gx = 0.0f, gy = 0.0f;
      field.gradientAt(nodes[ni].x, nodes[ni].y, gx, gy);
      const float cHere = field.clearanceAt(nodes[ni].x, nodes[ni].y);
      const float wallPush = (cHere < 24.0f) ? 0.54f : 0.14f;
      dx = dx * 0.68f + nodes[ni].dirX * 0.32f + gx * wallPush;
      dy = dy * 0.68f + nodes[ni].dirY * 0.32f + gy * wallPush;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len < 1e-3f) continue;
      dx /= len;
      dy /= len;

      const float nx = nodes[ni].x + dx * kStepLen;
      const float ny = nodes[ni].y + dy * kStepLen;
      if (field.clearanceAt(nx, ny) < 8.5f) continue;

      bool crowd = false;
      for (const auto& existing : nodes) {
        const float ex = nx - existing.x;
        const float ey = ny - existing.y;
        if (ex * ex + ey * ey < (kStepLen * 0.66f) * (kStepLen * 0.66f)) {
          crowd = true;
          break;
        }
      }
      if (!crowd) {
        nodes.push_back({nx, ny, static_cast<int>(ni), nodes[ni].depth + 1, dx, dy});
      }
    }
  }

  // Compute subtree weight & child counts so main trunks are thick and twigs taper gracefully!
  std::vector<int> childCount(nodes.size(), 0);
  std::vector<int> subtreeWeight(nodes.size(), 1);
  for (size_t i = nodes.size() - 1; i >= 1; --i) {
    const int p = nodes[i].parent;
    childCount[p]++;
    subtreeWeight[p] += subtreeWeight[i];
  }

  // Smooth interior non-branching vine vertices so stems flow in sinuous S-curves instead of stiff polygons!
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<std::pair<float, float>> nextPos(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) nextPos[i] = {nodes[i].x, nodes[i].y};
    for (size_t i = 1; i < nodes.size(); ++i) {
      if (childCount[i] != 1) continue;
      int onlyChild = -1;
      for (size_t j = i + 1; j < nodes.size(); ++j) {
        if (nodes[j].parent == static_cast<int>(i)) {
          onlyChild = static_cast<int>(j);
          break;
        }
      }
      if (onlyChild > 0) {
        const auto& par = nodes[nodes[i].parent];
        const auto& ch = nodes[onlyChild];
        const float sx = 0.25f * par.x + 0.50f * nodes[i].x + 0.25f * ch.x;
        const float sy = 0.25f * par.y + 0.50f * nodes[i].y + 0.25f * ch.y;
        if (field.clearanceAt(sx, sy) >= 8.5f) {
          nextPos[i] = {sx, sy};
        }
      }
    }
    for (size_t i = 1; i < nodes.size(); ++i) {
      nodes[i].x = nextPos[i].first;
      nodes[i].y = nextPos[i].second;
    }
  }

  // Track occupied focal zones (flowers & fauna) so nothing overlaps messily!
  struct OccupiedZone {
    float x, y, r;
  };
  std::vector<OccupiedZone> zones;
  auto isZoneFree = [&](float x, float y, float r, float pad = 4.0f) {
    for (const auto& z : zones) {
      if (std::hypot(x - z.x, y - z.y) < z.r + r + pad) return false;
    }
    return true;
  };

  // 3. Place Hero Blossoms at sunlit interior/canopy vine nodes (away from the grounded root)!
  int heroPlaced = 0;
  const int maxHeroes = isTallCanvas ? 5 : 3;
  for (size_t p = 0; p < reg.poles.size() && heroPlaced < maxHeroes; ++p) {
    const auto& rawPole = reg.poles[p];
    size_t bestIdx = 0;
    float bestD = 1e9f;
    for (size_t ni = 1; ni < nodes.size(); ++ni) {
      if (std::hypot(nodes[ni].x - rootX, nodes[ni].y - rootY) < 28.0f) continue;
      const float d = std::hypot(nodes[ni].x - rawPole.x, nodes[ni].y - rawPole.y);
      if (d < bestD) {
        bestD = d;
        bestIdx = ni;
      }
    }
    if (bestIdx == 0 || bestD > 68.0f) continue;

    float fx = nodes[bestIdx].x;
    float fy = nodes[bestIdx].y;
    const float desiredR =
        clampf(field.clearanceAt(fx, fy) * (heroPlaced == 0 ? 0.48f : 0.42f), 14.5f, heroPlaced == 0 ? 31.0f : 23.0f);
    float safeR = desiredR;
    if (!fitSafeBloomRadius(field, fx, fy, desiredR, 13.5f, safeR)) continue;
    if (!isZoneFree(fx, fy, safeR, 6.0f)) continue;

    // Connect vine node cleanly to nudged bloom center if needed.
    if (std::hypot(fx - nodes[bestIdx].x, fy - nodes[bestIdx].y) > 1.5f) {
      sg.addStroke(2, nodes[bestIdx].x, nodes[bestIdx].y, fx, fy, 1.2f, 0.9f, true);
    }

    // Backing guard leaves around hero bloom.
    const int nGuard = (heroPlaced == 0) ? 3 : 2;
    const float gRot = rng.range(0.0f, kTwoPi);
    for (int k = 0; k < nGuard; ++k) {
      const float la = gRot + (kTwoPi * k) / nGuard + rng.range(-0.2f, 0.2f);
      const float lx = fx + std::cos(la) * (safeR * 0.52f);
      const float ly = fy + std::sin(la) * (safeR * 0.52f);
      const float lLen = safeR * rng.range(0.62f, 0.80f);
      if (field.clearanceAt(lx + std::cos(la) * lLen, ly + std::sin(la) * lLen) >= 4.5f) {
        sg.addLeaf(3, lx, ly, la, lLen, lLen * 0.46f, (k % 2 == 0) ? 0.14f : -0.14f,
                   (k % 2 == 0) ? LeafStyle::WoodcutHalfShade : LeafStyle::HeartCordate, k % 2 == 0, rng.nextU32());
      }
    }

    // Distinct flower species for every hero bloom (dog-rose, daisy seedhead, peony, goblet tulip)!
    const int sp = (heroPlaced == 0) ? rng.rangeInt(0, 1) : ((heroPlaced + rng.rangeInt(1, 3)) % 5);
    const auto& par = nodes[nodes[bestIdx].parent];
    const float stemAng = std::atan2(nodes[bestIdx].y - par.y, nodes[bestIdx].x - par.x);
    emitBotanicalFlower(sg, fx, fy, safeR, sp, stemAng, rng);
    zones.push_back({fx, fy, safeR});
    ++heroPlaced;
  }

  // Helper: minimum distance from point (qx, qy) to any vine segment in the skeleton
  // (optionally skipping segments incident to excluded node indices aExcl / bExcl).
  auto minDistToVine = [&](float qx, float qy, int aExcl = -1, int bExcl = -1) {
    float bestD2 = 1e9f;
    for (size_t k = 1; k < nodes.size(); ++k) {
      const int pk = nodes[k].parent;
      if (static_cast<int>(k) == aExcl || static_cast<int>(k) == bExcl || pk == aExcl || pk == bExcl) continue;
      const float vx = nodes[k].x - nodes[pk].x;
      const float vy = nodes[k].y - nodes[pk].y;
      const float len2 = vx * vx + vy * vy;
      const float t =
          (len2 > 1e-5f) ? clampf(((qx - nodes[pk].x) * vx + (qy - nodes[pk].y) * vy) / len2, 0.0f, 1.0f) : 0.0f;
      const float dx = qx - (nodes[pk].x + t * vx);
      const float dy = qy - (nodes[pk].y + t * vy);
      const float d2 = dx * dx + dy * dy;
      if (d2 < bestD2) bestD2 = d2;
    }
    return std::sqrt(bestD2);
  };

  const bool isDeepRegion = (reg.y1 - reg.y0) >= 96 && reg.areaPx >= 20000;

  // 4. Garden Fauna #1: Hovering Hummingbird sipping from a side-facing Nectar Trumpet in clean open air!
  bool placedHummingbird = false;
  if (reg.areaPx >= 15000 && (reg.y1 - reg.y0) >= 68) {
    // Search vine nodes for a blossom attachment where open air beside it can host a hovering hummingbird!
    float bestBirdScore = -1.0f;
    size_t bestFlowerNode = 0;
    float bestBirdX = 0.0f, bestBirdY = 0.0f, bestScale = 26.0f, bestFacing = 1.0f;
    float bestTrBaseX = 0.0f, bestTrBaseY = 0.0f, bestTrAng = 0.0f, bestTrLen = 21.0f;

    const float tiltOffsets[3] = {-0.18f, 0.04f, -0.34f};
    for (size_t ni = 1; ni < nodes.size(); ++ni) {
      if (childCount[ni] > 1) continue;
      const float fx = nodes[ni].x;
      const float fy = nodes[ni].y;
      if (!isZoneFree(fx, fy, 11.0f, 2.0f)) continue;

      for (int sideDir = -1; sideDir <= 1; sideDir += 2) {
        for (float tilt : tiltOffsets) {
          // Trumpet aims outward toward open air; hummingbird faces back (-sideDir) into its corolla!
          const float trAng = (sideDir > 0) ? tilt : (kPi - tilt);
          const float trLen = 21.0f;
          const float mouthX = fx + std::cos(trAng) * trLen;
          const float mouthY = fy + std::sin(trAng) * trLen;

          const float birdScale =
              clampf(field.clearanceAt(mouthX + sideDir * 30.0f, mouthY - 5.0f) * 0.86f, 24.0f, 32.0f);
          const float facing = static_cast<float>(-sideDir);  // Bird faces toward the trumpet mouth!
          // Beak tip in local coords is at (+1.24 * S * facing, -0.20 * S); place bird so beak tip meets mouth!
          const float bx = mouthX - facing * (1.27f * birdScale);
          const float by = mouthY + 0.18f * birdScale;
          const float wingX = bx - facing * (0.82f * birdScale);
          const float wingY = by - 0.82f * birdScale;
          const float tailX = bx - facing * (0.78f * birdScale);
          const float tailY = by + 0.64f * birdScale;

          // Verify page/text clearance across the entire hummingbird silhouette!
          const float cCenter = field.clearanceAt(bx, by);
          const float cWing = field.clearanceAt(wingX, wingY);
          const float cTail = field.clearanceAt(tailX, tailY);
          const float cHead = field.clearanceAt(bx + facing * (0.40f * birdScale), by - 0.28f * birdScale);
          const float minC = std::min({cCenter - 7.5f, cWing - 2.5f, cTail - 2.5f, cHead - 3.5f});
          if (minC < 1.5f) continue;
          if (!isZoneFree(bx, by, birdScale * 0.96f, 4.0f)) continue;

          // Guarantee the hummingbird hovers in OPEN AIR — never impaled/crossed by a vine branch!
          const float stemDistBody = minDistToVine(bx, by);
          const float stemDistHead = minDistToVine(bx + facing * (0.36f * birdScale), by - 0.16f * birdScale);
          const float stemDistWing = minDistToVine(wingX, wingY);
          const float stemDistTail = minDistToVine(tailX, tailY);
          if (stemDistBody < birdScale * 0.56f || stemDistHead < birdScale * 0.44f || stemDistWing < 7.5f ||
              stemDistTail < 7.5f) {
            continue;
          }

          const float score = minC + std::min(stemDistBody, 28.0f) * 0.8f + birdScale * 0.5f + (reg.y1 - by) * 0.04f +
                              rng.range(0.0f, 3.5f);
          if (score > bestBirdScore) {
            bestBirdScore = score;
            bestFlowerNode = ni;
            bestBirdX = bx;
            bestBirdY = by;
            bestScale = birdScale;
            bestFacing = facing;
            bestTrBaseX = fx;
            bestTrBaseY = fy;
            bestTrAng = trAng;
            bestTrLen = trLen;
          }
        }
      }
    }

    if (bestFlowerNode > 0) {
      float mx = 0.0f, my = 0.0f;
      if (emitTrumpetFlower(sg, field, bestTrBaseX, bestTrBaseY, bestTrAng, bestTrLen, mx, my, rng)) {
        sg.addCreature(8, CreatureKind::Hummingbird, bestBirdX, bestBirdY, bestScale, -0.06f * bestFacing, bestFacing,
                       rng.nextU32());
        zones.push_back({bestBirdX, bestBirdY, bestScale * 1.12f});
        zones.push_back({(bestTrBaseX + mx) * 0.5f, (bestTrBaseY + my) * 0.5f, bestTrLen * 0.72f});
        zones.push_back({bestTrBaseX, bestTrBaseY, 4.0f});
        placedHummingbird = true;
      }
    }
  }

  // 5. Garden Fauna #2: Tree Frog resting on a sturdy gently-sloping interior vine branch!
  if (reg.areaPx >= 14000 && (isDeepRegion || !placedHummingbird)) {
    float bestFrogScore = -1.0f;
    float frogX = 0.0f, frogY = 0.0f, frogScale = 26.0f, frogAng = 0.0f, frogFacing = 1.0f;
    bool foundFrogPerch = false;

    for (size_t ni = 2; ni < nodes.size(); ++ni) {
      if (childCount[ni] == 0) continue;  // Prefer sturdy interior vine limbs, not thin tips!
      const auto& cur = nodes[ni];
      const auto& par = nodes[cur.parent];
      float dx = cur.x - par.x;
      float dy = cur.y - par.y;
      if (std::hypot(dx, dy) < 10.5f) continue;

      // Orient segment left-to-right so 'angle' is in [-pi/2, +pi/2] and -v always points UPWARD above the vine!
      if (dx < 0.0f) {
        dx = -dx;
        dy = -dy;
      }
      const float slopeAng = std::atan2(dy, dx);
      if (std::fabs(slopeAng) > 0.62f) continue;  // Frog wants a comfortable horizontal/gently-sloping perch!

      const float px = 0.5f * (par.x + cur.x);
      const float py = 0.5f * (par.y + cur.y);
      const float upX = px + std::sin(slopeAng) * 12.5f;
      const float upY = py - std::cos(slopeAng) * 12.5f;
      const float cAbove = field.clearanceAt(upX, upY);
      if (cAbove < 17.0f) continue;

      const float fScale = clampf(cAbove * 0.96f, 23.0f, 30.5f);
      if (!isZoneFree(upX, upY, fScale * 0.82f, 6.0f)) continue;
      // Ensure no overhead vine branch slices across the frog's back!
      if (minDistToVine(upX, upY, static_cast<int>(ni), cur.parent) < fScale * 0.42f) continue;
      // Ensure raised head/snout and rump also have comfortable clearance!
      if (field.clearanceAt(upX + fScale * 0.55f, upY - fScale * 0.25f) < 5.5f ||
          field.clearanceAt(upX - fScale * 0.55f, upY - fScale * 0.15f) < 5.5f) {
        continue;
      }

      const float score = cAbove * 1.2f + subtreeWeight[ni] * 0.35f + rng.range(0.0f, 5.0f);
      if (score > bestFrogScore) {
        bestFrogScore = score;
        frogX = px;
        frogY = py;
        frogScale = fScale;
        frogAng = slopeAng;
        frogFacing = (px < (reg.x0 + reg.x1) * 0.5f) ? 1.0f : -1.0f;
        foundFrogPerch = true;
      }
    }

    if (foundFrogPerch) {
      sg.addCreature(8, CreatureKind::TreeFrog, frogX, frogY, frogScale, frogAng, frogFacing, rng.nextU32());
      zones.push_back({frogX, frogY - frogScale * 0.36f, frogScale * 1.08f});
    }
  }

  // 6. Emit tapered woody-to-slender stems, BILATERAL leaves (both sides of the vine!), diverse flowers, ferns &
  // berries!
  const float maxW = std::sqrt(static_cast<float>(std::max(1, subtreeWeight[0])));
  std::vector<int> nodeLeafSign(nodes.size(), 1);
  nodeLeafSign[0] = rng.chance(0.5f) ? 1 : -1;

  for (size_t i = 1; i < nodes.size(); ++i) {
    const auto& cur = nodes[i];
    const auto& par = nodes[cur.parent];
    // Prune tiny childless stub twigs whose tip pokes into an occupied blossom/creature halo!
    if (childCount[i] == 0 && static_cast<int>(i) != -1 && !isZoneFree(cur.x, cur.y, 1.0f, 0.0f)) {
      // Keep the twig only if a flower/trumpet was explicitly attached right at node i!
      bool isExactAnchor = false;
      for (const auto& z : zones) {
        if (std::hypot(cur.x - z.x, cur.y - z.y) < 3.0f) isExactAnchor = true;
      }
      if (!isExactAnchor) continue;
    }
    const bool placeLeavesHere = (cur.depth % 2 == 1) || (childCount[i] > 1);
    nodeLeafSign[i] = placeLeavesHere ? -nodeLeafSign[cur.parent] : nodeLeafSign[cur.parent];

    const float w0 = std::sqrt(static_cast<float>(subtreeWeight[cur.parent])) / maxW;
    const float w1 = std::sqrt(static_cast<float>(subtreeWeight[i])) / maxW;
    const float r0 = lerpf(0.58f, 2.35f, std::pow(w0, 0.65f));
    const float r1 = lerpf(0.52f, 2.35f, std::pow(w1, 0.65f));
    sg.addStroke(2, par.x, par.y, cur.x, cur.y, r0, r1, true, false, false, static_cast<uint32_t>(i));

    const float segAng = std::atan2(cur.y - par.y, cur.x - par.x);
    const float localClear = field.clearanceAt(cur.x, cur.y);

    // Emit airy foliage on BOTH sides of the vine (alternating Left/Right + 58% paired opposite leaves)!
    if (placeLeavesHere && localClear >= 11.0f && isZoneFree(cur.x, cur.y, 7.5f, 1.0f)) {
      const bool oppositePair = rng.chance(0.58f);
      const int passes = oppositePair ? 2 : 1;
      for (int k = 0; k < passes; ++k) {
        const float side = static_cast<float>((k == 0) ? nodeLeafSign[i] : -nodeLeafSign[i]);
        const float leafAng = segAng + side * rng.range(0.62f, 1.05f);
        const float leafLen = clampf(localClear * rng.range(0.62f, 0.86f), 10.5f, 24.0f);
        // Short curved petiole stem connecting vine to leaf base!
        const float petLen = clampf(leafLen * 0.20f, 2.5f, 4.5f);
        const float lx = cur.x + std::cos(leafAng - side * 0.18f) * petLen;
        const float ly = cur.y + std::sin(leafAng - side * 0.18f) * petLen;
        const float tipX = lx + std::cos(leafAng) * leafLen;
        const float tipY = ly + std::sin(leafAng) * leafLen;
        if (field.clearanceAt(tipX, tipY) < 4.0f) continue;

        if (!oppositePair && localClear >= 16.0f && rng.chance(0.14f)) {
          emitFernFrond(sg, field, cur.x, cur.y, leafAng, leafLen * 1.15f, side, rng);
        } else {
          sg.addStroke(2, cur.x, cur.y, lx, ly, 0.65f, 0.52f, false);
          const int styleRoll = rng.rangeInt(0, 9);
          LeafStyle ls = LeafStyle::WoodcutHalfShade;
          float widMul = rng.range(0.38f, 0.46f);
          bool serr = true;
          if (styleRoll < 3) {
            ls = LeafStyle::HeartCordate;
            widMul = rng.range(0.56f, 0.68f);
            serr = false;
          } else if (styleRoll < 5) {
            ls = LeafStyle::ScallopedOak;
            widMul = rng.range(0.46f, 0.56f);
            serr = false;
          }
          sg.addLeaf(3, lx, ly, leafAng, leafLen, leafLen * widMul, side * 0.15f, ls, serr, rng.nextU32());
        }
      }
    }

    // Terminal shoot accents: diverse side bellflowers, trumpets, wildflower species, berry clusters, buds & tendrils!
    if (childCount[i] == 0 && isZoneFree(cur.x, cur.y, 10.0f, 2.0f)) {
      const int accentRoll = rng.rangeInt(0, 99);
      if (localClear >= 16.0f && accentRoll < 26) {
        // Pendulous hanging Bellflower!
        const float bellL = clampf(localClear * 0.68f, 12.0f, 19.0f);
        if (emitBellFlower(sg, field, cur.x, cur.y, segAng, bellL, rng)) {
          zones.push_back({cur.x, cur.y + bellL * 0.5f, bellL * 0.65f});
          continue;
        }
      }
      if (localClear >= 16.5f && accentRoll < 44) {
        // Side-facing Nectar Trumpet blossom!
        float mx = 0.0f, my = 0.0f;
        const float trL = clampf(localClear * 0.72f, 13.0f, 19.5f);
        if (emitTrumpetFlower(sg, field, cur.x, cur.y, segAng, trL, mx, my, rng)) {
          zones.push_back({(cur.x + mx) * 0.5f, (cur.y + my) * 0.5f, trL * 0.60f});
          continue;
        }
      }
      if (localClear >= 17.5f && accentRoll < 68) {
        float fx = cur.x, fy = cur.y, safeR = 14.0f;
        const float wantR = clampf(localClear * 0.54f, 12.0f, 19.5f);
        if (fitSafeBloomRadius(field, fx, fy, wantR, 11.5f, safeR) && isZoneFree(fx, fy, safeR, 3.0f)) {
          if (std::hypot(fx - cur.x, fy - cur.y) > 1.2f) {
            sg.addStroke(2, cur.x, cur.y, fx, fy, 0.85f, 0.65f, false);
          }
          emitBotanicalFlower(sg, fx, fy, safeR, rng.rangeInt(0, 4), segAng, rng);
          zones.push_back({fx, fy, safeR});
          continue;
        }
      }
      if (localClear >= 12.5f && accentRoll < 80) {
        emitBerryCluster(sg, field, cur.x, cur.y, segAng, clampf(localClear * 0.58f, 8.5f, 12.5f), rng);
      } else if (localClear >= 11.5f && accentRoll < 90) {
        emitRoseBud(sg, cur.x, cur.y, segAng, clampf(localClear * 0.62f, 8.5f, 14.0f), rng);
      } else if (localClear >= 8.5f) {
        emitSpiralTendril(sg, field, cur.x, cur.y, segAng, clampf(localClear * 0.85f, 9.0f, 17.5f),
                          static_cast<float>(nodeLeafSign[i]), 2);
      }
    }
  }
  (void)placedHummingbird;
}

// ============================================================================
// Theme 2: Classical Ink-Wash Plum Blossom & Bamboo (水墨梅竹・远山云烟)
// ============================================================================

void buildPlumAndBamboo(SceneGraph& sg, const ClearanceField& field, const DeadRegion& reg, Rng& rng) {
  if (reg.poles.empty()) return;
  const Pole& mainPole = reg.poles[0];

  if (reg.kind == RegionKind::SceneBreakCorridor) {
    const float cy = mainPole.y;
    const float cx = (reg.x0 + reg.x1) * 0.5f;
    const float span = std::min(165.0f, (reg.x1 - reg.x0) * 0.42f);
    const float blossomR = clampf(mainPole.clearance * 0.66f, 8.0f, 13.0f);

    // Calligraphic angular plum twig across the scene break.
    std::vector<std::pair<float, float>> twig = {{cx - span, cy + 3.0f},
                                                 {cx - span * 0.45f, cy - 2.5f},
                                                 {cx - span * 0.1f, cy + 2.0f},
                                                 {cx + span * 0.38f, cy - 3.0f},
                                                 {cx + span * 0.92f, cy + 1.5f}};
    emitCurvedPolyline(sg, 2, twig, 2.1f, 0.7f, true, true, rng.nextU32());
    float b1x = cx - span * 0.14f, b1y = cy + 1.5f, r1 = blossomR;
    if (fitSafeBloomRadius(field, b1x, b1y, blossomR, 7.5f, r1)) {
      emitPlumBlossom(sg, b1x, b1y, r1, rng);
    }
    float b2x = cx + span * 0.36f, b2y = cy - 2.0f, r2 = blossomR * 0.86f;
    if (fitSafeBloomRadius(field, b2x, b2y, blossomR * 0.86f, 7.5f, r2)) {
      emitPlumBlossom(sg, b2x, b2y, r2, rng);
    }
    // Attached plum buds with ink pedicels + calyx cups (no floating dots!).
    emitAttachedPlumBud(sg, field, cx - span * 0.45f, cy - 2.5f, -2.35f, 5.0f, blossomR * 0.32f, rng);
    emitAttachedPlumBud(sg, field, cx + span * 0.65f, cy - 0.8f, -0.55f, 5.0f, blossomR * 0.30f, rng);
    return;
  }

  // Distant Shan-Shui mountain mist wash in wide tailpieces or tall multi-bay sleep canvases!
  if ((reg.kind == RegionKind::TailpieceExpanse && (reg.y1 - reg.y0) >= 120) || (reg.y1 - reg.y0) > 460) {
    sg.addWash(reg.x0, reg.y0 + (reg.y1 - reg.y0) / 4, reg.x1, reg.y1, 0, 178, rng.nextU32());
  }

  auto minSegClear = [&](float x0, float y0, float x1, float y1) {
    float mc = 1e6f;
    for (int k = 0; k <= 14; ++k) {
      const float t = static_cast<float>(k) / 14.0f;
      mc = std::min(mc, field.clearanceAt(lerpf(x0, x1, t), lerpf(y0, y1, t)));
    }
    return mc;
  };

  // Track placed plum blossoms so overlapping clusters keep clean petal readability!
  struct PlacedPlum {
    float x, y, r;
  };
  std::vector<PlacedPlum> placedBlossoms;
  auto canPlacePlum = [&](float x, float y, float r) {
    for (const auto& pb : placedBlossoms) {
      if (std::hypot(x - pb.x, y - pb.y) < (pb.r + r) * 0.72f) return false;
    }
    return true;
  };

  // Reserve mainPole hero blossom first so every petal has guaranteed clearance!
  float heroX = mainPole.x, heroY = mainPole.y, heroR = 18.0f;
  const bool hasHero =
      fitSafeBloomRadius(field, heroX, heroY, clampf(mainPole.clearance * 0.42f, 13.5f, 23.0f), 11.0f, heroR);
  if (hasHero) {
    placedBlossoms.push_back({heroX, heroY, heroR});
  }

  // Choose an anchor side whose trunk segment to heroX/heroY never clips across a card corner!
  auto findCleanTrunkAnchor = [&](float tx, float ty, bool preferLeft, float& outX, float& outY, bool& outLeft) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      const bool sideLeft = (attempt == 0) ? preferLeft : !preferLeft;
      float sx = sideLeft ? static_cast<float>(reg.x0 + 16) : static_cast<float>(reg.x1 - 16);
      float sy =
          clampf(ty + (sideLeft ? 48.0f : 38.0f), static_cast<float>(reg.y0 + 18), static_cast<float>(reg.y1 - 18));
      for (int s = 0; s < 16 && (field.clearanceAt(sx, sy) < 11.5f || minSegClear(sx, sy, tx, ty) < 9.5f); ++s) {
        sx = lerpf(sx, tx, 0.16f);
        sy = lerpf(sy, ty, 0.16f);
      }
      if (minSegClear(sx, sy, tx, ty) >= 8.5f) {
        outX = sx;
        outY = sy;
        outLeft = sideLeft;
        return;
      }
    }
    outLeft = (tx < field.screenW * 0.5f);
    outX = lerpf(outLeft ? static_cast<float>(reg.x0 + 16) : static_cast<float>(reg.x1 - 16), tx, 0.55f);
    outY = lerpf(ty + 30.0f, ty, 0.55f);
  };

  bool fromLeft = (mainPole.x < field.screenW * 0.5f);
  float startX = heroX, startY = heroY;
  findCleanTrunkAnchor(heroX, heroY, fromLeft, startX, startY, fromLeft);

  // Recursive angular brush branch builder ("iron-wire" calligraphy kinks + feibai dry brush).
  auto growPlumBranch = [&](auto& self, float x, float y, float ang, float segLen, float radius, int depth) -> void {
    if (depth > 6 || segLen < 10.0f) return;
    float nx = x + std::cos(ang) * segLen;
    float ny = y + std::sin(ang) * segLen;
    float c = field.clearanceAt(nx, ny);
    if (c < 8.0f) {
      float gx = 0.0f, gy = 0.0f;
      field.gradientAt(x, y, gx, gy);
      ang = std::atan2(std::sin(ang) * 0.45f + gy * 0.55f, std::cos(ang) * 0.45f + gx * 0.55f);
      nx = x + std::cos(ang) * (segLen * 0.72f);
      ny = y + std::sin(ang) * (segLen * 0.72f);
      c = field.clearanceAt(nx, ny);
      if (c < 7.5f) return;
    }

    const float nextR = std::max(0.65f, radius * 0.74f);
    sg.addStroke(2, x, y, nx, ny, radius, nextR, true, radius >= 2.1f, false, rng.nextU32());

    // Complete 5-petal blossoms (verified around full perimeter so zero petals are ever clipped!)
    // and physically attached pedicel+calyx pearl buds (zero floating dots!).
    bool spawnedBloom = false;
    if (depth >= 2 && c >= 15.5f && rng.chance(depth >= 3 ? 0.68f : 0.45f)) {
      float bx = nx, by = ny, safeR = 12.0f;
      const float wantR = clampf(c * rng.range(0.36f, 0.48f), 10.5f, 19.5f);
      if (fitSafeBloomRadius(field, bx, by, wantR, 10.0f, safeR) && canPlacePlum(bx, by, safeR)) {
        if (std::hypot(bx - nx, by - ny) > 1.2f) {
          sg.addStroke(2, nx, ny, bx, by, 0.85f, 0.70f, false);
        }
        emitPlumBlossom(sg, bx, by, safeR, rng);
        placedBlossoms.push_back({bx, by, safeR});
        spawnedBloom = true;
      }
    }
    if (!spawnedBloom && depth >= 2 && c >= 10.5f && rng.chance(0.58f)) {
      const float budAng = ang + (rng.chance(0.5f) ? 0.68f : -0.68f);
      emitAttachedPlumBud(sg, field, nx, ny, budAng, rng.range(4.8f, 6.8f), rng.range(3.6f, 4.9f), rng);
    }

    // Calligraphic angular bend + fork!
    const float kink = (depth % 2 == 0 ? 1.0f : -1.0f) * rng.range(0.24f, 0.46f);
    self(self, nx, ny, ang + kink, segLen * rng.range(0.78f, 0.88f), nextR, depth + 1);
    if (depth <= 4 && rng.chance(0.70f)) {
      const float forkAng = ang - kink * rng.range(1.4f, 2.0f);
      self(self, nx, ny, forkAng, segLen * rng.range(0.62f, 0.78f), nextR * 0.82f, depth + 2);
    }
  };

  const float mainAng = std::atan2(heroY - startY, heroX - startX);
  const float trunkDist = std::hypot(heroX - startX, heroY - startY);
  const float trunkLen = clampf(trunkDist * 0.45f, 26.0f, 56.0f);
  const float midX = lerpf(startX, heroX, 0.52f) + (fromLeft ? 8.0f : -8.0f);
  const float midY = lerpf(startY, heroY, 0.52f) + 10.0f;
  if (field.clearanceAt(midX, midY) >= 9.0f && minSegClear(startX, startY, midX, midY) >= 8.5f &&
      minSegClear(midX, midY, heroX, heroY) >= 8.5f) {
    sg.addStroke(2, startX, startY, midX, midY, 3.6f, 2.7f, true, true, false, rng.nextU32());
    sg.addStroke(2, midX, midY, heroX, heroY, 2.7f, 2.0f, true, true, false, rng.nextU32());
    growPlumBranch(growPlumBranch, midX, midY, mainAng - 0.52f, trunkLen * 0.72f, 2.1f, 2);
  } else {
    sg.addStroke(2, startX, startY, heroX, heroY, 3.3f, 2.1f, true, true, false, rng.nextU32());
  }
  growPlumBranch(growPlumBranch, heroX, heroY, mainAng + 0.22f, trunkLen * 0.86f, 2.1f, 1);
  growPlumBranch(growPlumBranch, heroX, heroY, mainAng - 0.58f, trunkLen * 0.72f, 1.7f, 2);

  if (hasHero) {
    emitPlumBlossom(sg, heroX, heroY, heroR, rng);
  }

  // On full-height multi-bay canvases (e.g. staggered sleep-screen vocabulary cards),
  // grow up to 2 companion plum boughs into distant upper/lower bays!
  if ((reg.y1 - reg.y0) > 460 && reg.poles.size() >= 3) {
    int companionCount = 0;
    float lastAltY = mainPole.y;
    for (size_t pi = 1; pi < reg.poles.size() && companionCount < 2; ++pi) {
      const Pole& altPole = reg.poles[pi];
      if (std::fabs(altPole.y - mainPole.y) < 175.0f || std::fabs(altPole.y - lastAltY) < 175.0f ||
          altPole.clearance < 19.0f) {
        continue;
      }
      bool altLeft = (altPole.x < field.screenW * 0.5f);
      float ax0 = altPole.x, ay0 = altPole.y;
      findCleanTrunkAnchor(altPole.x, altPole.y, altLeft, ax0, ay0, altLeft);
      float ahx = altPole.x, ahy = altPole.y, ahr = 16.0f;
      if (fitSafeBloomRadius(field, ahx, ahy, clampf(altPole.clearance * 0.42f, 12.5f, 21.0f), 10.5f, ahr) &&
          canPlacePlum(ahx, ahy, ahr)) {
        placedBlossoms.push_back({ahx, ahy, ahr});
        sg.addStroke(2, ax0, ay0, ahx, ahy, 3.1f, 1.9f, true, true, false, rng.nextU32());
        const float aAng = std::atan2(ahy - ay0, ahx - ax0);
        const float aLen = clampf(std::hypot(ahx - ax0, ahy - ay0) * 0.48f, 24.0f, 48.0f);
        growPlumBranch(growPlumBranch, ahx, ahy, aAng + 0.24f, aLen * 0.84f, 1.9f, 1);
        growPlumBranch(growPlumBranch, ahx, ahy, aAng - 0.56f, aLen * 0.68f, 1.5f, 2);
        emitPlumBlossom(sg, ahx, ahy, ahr, rng);
        lastAltY = altPole.y;
        ++companionCount;
      }
    }
  }

  // Add graceful segmented Ink Bamboo (墨竹) stalk + swept leaf clusters on secondary pole or tall bay!
  const Pole& bambooPole = (reg.poles.size() >= 2) ? reg.poles[1] : mainPole;
  if (reg.poles.size() >= 2 || reg.kind == RegionKind::MarginalBay) {
    float bx = (reg.poles.size() >= 2) ? bambooPole.x : (mainPole.x + (fromLeft ? 34.0f : -34.0f));
    float by = std::min(static_cast<float>(reg.y1 - 16), bambooPole.y + bambooPole.clearance * 0.85f);
    while (by > bambooPole.y && field.clearanceAt(bx, by) < 9.0f) by -= 8.0f;

    float curAng = -kPi * 0.5f + (fromLeft ? -0.14f : 0.14f);
    const int nSegs = std::clamp(static_cast<int>((reg.y1 - reg.y0) / 42), 3, 7);
    for (int s = 0; s < nSegs; ++s) {
      const float segH = rng.range(26.0f, 36.0f);
      const float nx = bx + std::cos(curAng) * segH;
      const float ny = by + std::sin(curAng) * segH;
      if (field.clearanceAt(nx, ny) < 8.0f) break;

      const float rCulm = lerpf(2.2f, 1.05f, static_cast<float>(s) / nSegs);
      const float gapRatio = 2.4f / segH;
      const float sx1 = lerpf(bx, nx, 1.0f - gapRatio);
      const float sy1 = lerpf(by, ny, 1.0f - gapRatio);
      sg.addStroke(2, bx, by, sx1, sy1, rCulm, rCulm * 0.92f, true, false, false, rng.nextU32());
      const float px = -std::sin(curAng) * (rCulm + 2.2f);
      const float py = std::cos(curAng) * (rCulm + 2.2f);
      sg.addStroke(3, sx1 - px, sy1 - py, sx1 + px, sy1 + py, 0.85f, 0.85f, false);

      if (s >= 1 && field.clearanceAt(sx1, sy1) >= 14.0f) {
        const float side = (s % 2 == 0) ? 1.0f : -1.0f;
        const float baseLeafAng = (side > 0 ? 0.32f : (kPi - 0.32f));
        for (int k = 0; k < 3; ++k) {
          const float la = baseLeafAng + (k - 1) * 0.34f + rng.range(-0.06f, 0.06f);
          const float lLen = clampf(field.clearanceAt(sx1, sy1) * rng.range(0.75f, 1.05f), 16.0f, 36.0f);
          if (field.clearanceAt(sx1 + std::cos(la) * lLen, sy1 + std::sin(la) * lLen) >= 4.5f) {
            sg.addLeaf(k == 0 ? 1 : 3, sx1, sy1, la, lLen, lLen * 0.17f, 0.08f * side,
                       (k == 0) ? LeafStyle::InkWashBamboo : LeafStyle::SolidInkBamboo, false, rng.nextU32());
          }
        }
      }

      bx = nx;
      by = ny;
      curAng += (fromLeft ? -0.04f : 0.04f);
    }
  }
}

// ============================================================================
// Theme 3: Celestial Astrolabe & Antique Star Atlas (Refined Open Asterisms)
// ============================================================================

void buildCelestialAstrolabe(SceneGraph& sg, const ClearanceField& field, const DeadRegion& reg,
                             const std::vector<int16_t>& cellComp, Rng& rng) {
  if (reg.poles.empty()) return;
  const Pole& mainPole = reg.poles[0];

  if (reg.kind == RegionKind::SceneBreakCorridor) {
    const float cy = mainPole.y;
    const float cx = (reg.x0 + reg.x1) * 0.5f;
    const float moonR = clampf(mainPole.clearance * 0.65f, 7.0f, 11.5f);
    const float spacing = moonR * 3.3f;
    const float phases[5] = {-2.15f, -1.10f, 0.0f, 1.10f, 2.15f};
    const float axisHalf = std::min(165.0f, (reg.x1 - reg.x0) * 0.42f);
    sg.addStroke(1, cx - axisHalf, cy, cx + axisHalf, cy, 0.52f, 0.52f, false, false, true);
    for (int i = 0; i < 5; ++i) {
      const float mx = cx + (i - 2) * spacing;
      if (field.clearanceAt(mx, cy) >= moonR + 2.5f) {
        const float rHere = (i == 2) ? moonR * 1.15f : moonR * 0.86f;
        sg.addDisc(6, mx, cy, rHere, DiscKind::Moon3D, phases[i], 30, 250, rng.nextU32());
      }
    }
    sg.addDisc(7, cx - axisHalf, cy, 6.5f, DiscKind::CelestialStar4, 0.0f);
    sg.addDisc(7, cx + axisHalf, cy, 6.5f, DiscKind::CelestialStar4, 0.0f);
    return;
  }

  // Smooth subtle Milky Way stardust sweep behind the star chart!
  sg.addWash(reg.x0, reg.y0, reg.x1, reg.y1, 1, 198, rng.nextU32());

  const float astroR = clampf(mainPole.clearance * 0.58f, 19.0f, 48.0f);

  auto emitClippedCircle = [&](float cx, float cy, float r, float strokeR, bool dotted, int segs = 84) {
    for (int i = 0; i < segs; ++i) {
      if (dotted && (i % 2 == 1)) continue;
      const float a0 = (kTwoPi * i) / segs;
      const float a1 = (kTwoPi * (i + 1)) / segs;
      const float x0 = cx + std::cos(a0) * r;
      const float y0 = cy + std::sin(a0) * r;
      const float x1 = cx + std::cos(a1) * r;
      const float y1 = cy + std::sin(a1) * r;
      if (field.clearanceAt(x0, y0) >= 6.0f && field.clearanceAt(x1, y1) >= 6.0f) {
        sg.addStroke(2, x0, y0, x1, y1, strokeR, strokeR, false);
      }
    }
  };

  // Graduated outer & inner astrolabe degree rings + single outer celestial equator arc.
  emitClippedCircle(mainPole.x, mainPole.y, astroR, 1.05f, false, 84);
  emitClippedCircle(mainPole.x, mainPole.y, astroR * 0.86f, 0.62f, false, 72);
  emitClippedCircle(mainPole.x, mainPole.y, astroR * 1.48f, 0.52f, true, 108);
  // Off-center ecliptic ring inside astrolabe.
  emitClippedCircle(mainPole.x + astroR * 0.13f, mainPole.y - astroR * 0.10f, astroR * 0.65f, 0.58f, false, 64);

  // 36 degree graduation ticks around the astrolabe bezel.
  for (int i = 0; i < 36; ++i) {
    const float a = (kTwoPi * i) / 36.0f;
    const float innerR = (i % 3 == 0) ? astroR * 0.80f : astroR * 0.88f;
    sg.addStroke(3, mainPole.x + std::cos(a) * innerR, mainPole.y + std::sin(a) * innerR,
                 mainPole.x + std::cos(a) * astroR, mainPole.y + std::sin(a) * astroR, 0.58f, 0.58f, false);
  }

  // Diagonal engraved Alidade sight-rule across the astrolabe!
  const float alidadeAng = -0.58f;
  const float alR = astroR * 1.14f;
  if (field.clearanceAt(mainPole.x + std::cos(alidadeAng) * alR, mainPole.y + std::sin(alidadeAng) * alR) >= 5.0f &&
      field.clearanceAt(mainPole.x - std::cos(alidadeAng) * alR, mainPole.y - std::sin(alidadeAng) * alR) >= 5.0f) {
    sg.addStroke(5, mainPole.x - std::cos(alidadeAng) * alR, mainPole.y - std::sin(alidadeAng) * alR,
                 mainPole.x + std::cos(alidadeAng) * alR, mainPole.y + std::sin(alidadeAng) * alR, 0.68f, 0.68f, true);
  }

  // 4 cardinal needle pointers.
  for (int k = 0; k < 4; ++k) {
    const float a = k * kPi * 0.5f;
    const float sx = mainPole.x + std::cos(a) * (astroR * 1.05f);
    const float sy = mainPole.y + std::sin(a) * (astroR * 1.05f);
    if (field.clearanceAt(sx, sy) >= 8.0f) {
      sg.addDisc(7, sx, sy, clampf(astroR * 0.18f, 5.5f, 8.5f), DiscKind::FacetedCompassStar, a);
    }
  }

  // Focal 3D Lambertian cratered Moon at center of astrolabe!
  const float phaseChoices[4] = {-1.20f, -0.60f, 0.65f, 1.30f};
  sg.addDisc(6, mainPole.x, mainPole.y, astroR * 0.44f, DiscKind::Moon3D, phaseChoices[rng.rangeInt(0, 3)], 25, 252,
             rng.nextU32());

  // Place a miniature ringed Saturn along the outer orbital arc if clearance allows!
  float saturnX = -999.0f, saturnY = -999.0f;
  for (int k = 0; k < 10; ++k) {
    const float sa = rng.range(0.0f, kTwoPi);
    const float sx = mainPole.x + std::cos(sa) * (astroR * 1.48f);
    const float sy = mainPole.y + std::sin(sa) * (astroR * 1.48f);
    if (field.clearanceAt(sx, sy) >= 19.0f) {
      sg.addDisc(7, sx, sy, 6.8f, DiscKind::RingedPlanet, -0.38f, 45, 245, rng.nextU32());
      saturnX = sx;
      saturnY = sy;
      break;
    }
  }

  // Subtle wide celestial declination coordinate arc sweeping across open sky!
  for (int i = 0; i < 144; ++i) {
    if (i % 3 != 0) continue;
    const float rDecl = astroR * 2.65f;
    const float a0 = (kTwoPi * i) / 144.0f;
    const float a1 = (kTwoPi * (i + 0.85f)) / 144.0f;
    const float x0 = mainPole.x + std::cos(a0) * rDecl;
    const float y0 = mainPole.y + std::sin(a0) * rDecl;
    const float x1 = mainPole.x + std::cos(a1) * rDecl;
    const float y1 = mainPole.y + std::sin(a1) * rDecl;
    const int gx = std::clamp(static_cast<int>(x0) / kCellPx, 0, field.gw - 1);
    const int gy = std::clamp(static_cast<int>(y0) / kCellPx, 0, field.gh - 1);
    if (cellComp[gy * field.gw + gx] == reg.componentId && field.clearanceAt(x0, y0) >= 10.0f &&
        field.clearanceAt(x1, y1) >= 10.0f) {
      sg.addStroke(1, x0, y0, x1, y1, 0.45f, 0.45f, false);
    }
  }

  // Build 2 to 6 DISTINCT Open Constellation Asterisms (dipper / kite / cross / arc figures via Prim's MST,
  // NEVER a messy triangulated cobweb!).
  struct StarPt {
    float x, y;
    float mag;
  };
  std::vector<std::vector<StarPt>> constellations;
  const int wantClusters = std::clamp(reg.areaPx / 15000, 2, 6);

  auto isSkySpotValid = [&](float sx, float sy, float minClear) {
    const int gx = std::clamp(static_cast<int>(sx) / kCellPx, 0, field.gw - 1);
    const int gy = std::clamp(static_cast<int>(sy) / kCellPx, 0, field.gh - 1);
    if (cellComp[gy * field.gw + gx] != reg.componentId) return false;
    if (field.clearanceAt(sx, sy) < minClear) return false;
    // Keep stars cleanly outside the outer dashed orbital ring (astroR * 1.48)!
    if (std::hypot(sx - mainPole.x, sy - mainPole.y) < astroR * 1.66f) return false;
    if (std::hypot(sx - saturnX, sy - saturnY) < 26.0f) return false;
    return true;
  };

  for (int c = 0; c < wantClusters * 12 && static_cast<int>(constellations.size()) < wantClusters; ++c) {
    const float seedX = rng.range(static_cast<float>(reg.x0 + 18), static_cast<float>(reg.x1 - 18));
    const float seedY = rng.range(static_cast<float>(reg.y0 + 18), static_cast<float>(reg.y1 - 18));
    if (!isSkySpotValid(seedX, seedY, 13.0f)) continue;

    bool nearOtherCluster = false;
    for (const auto& prevC : constellations) {
      for (const auto& st : prevC) {
        if (std::hypot(seedX - st.x, seedY - st.y) < 54.0f) {
          nearOtherCluster = true;
          break;
        }
      }
      if (nearOtherCluster) break;
    }
    if (nearOtherCluster) continue;

    // Grow an articulated 4-to-6 star asterism around (seedX, seedY)!
    std::vector<StarPt> group;
    group.push_back({seedX, seedY, 0.95f});  // Alpha star of constellation!
    const int wantStars = rng.rangeInt(4, 6);
    float walkAng = rng.range(0.0f, kTwoPi);

    for (int s = 1; s < wantStars; ++s) {
      bool added = false;
      const StarPt& anchor = group[rng.chance(0.75f) ? (group.size() - 1) : 0];
      for (int tries = 0; tries < 14; ++tries) {
        const float stepD = rng.range(24.0f, 44.0f);
        const float a = walkAng + rng.range(-0.95f, 0.95f);
        const float nx = anchor.x + std::cos(a) * stepD;
        const float ny = anchor.y + std::sin(a) * stepD;
        if (!isSkySpotValid(nx, ny, 9.5f)) continue;
        if (!isSkySpotValid(0.5f * (anchor.x + nx), 0.5f * (anchor.y + ny), 7.5f)) continue;

        bool clash = false;
        for (const auto& g : group) {
          if (std::hypot(nx - g.x, ny - g.y) < 21.0f) clash = true;
        }
        for (const auto& prevC : constellations) {
          for (const auto& g : prevC) {
            if (std::hypot(nx - g.x, ny - g.y) < 42.0f) clash = true;
          }
        }
        if (!clash) {
          walkAng = a;
          group.push_back({nx, ny, (s == 1) ? 0.72f : rng.range(0.25f, 0.62f)});
          added = true;
          break;
        }
      }
      if (!added) break;
    }

    if (group.size() >= 3) {
      constellations.push_back(std::move(group));
    }
  }

  // Connect each constellation cleanly via Prim's Minimum Spanning Tree (degree <= 3) + optional dipper bowl edge!
  for (const auto& group : constellations) {
    const size_t n = group.size();
    std::vector<bool> inTree(n, false);
    std::vector<int> degree(n, 0);
    inTree[0] = true;
    std::vector<std::pair<size_t, size_t>> edges;

    for (size_t step = 1; step < n; ++step) {
      float bestD = 1e9f;
      size_t bestU = 0, bestV = 0;
      bool found = false;
      for (size_t u = 0; u < n; ++u) {
        if (!inTree[u] || degree[u] >= 3) continue;
        for (size_t v = 0; v < n; ++v) {
          if (inTree[v]) continue;
          const float d = std::hypot(group[v].x - group[u].x, group[v].y - group[u].y);
          if (d < bestD) {
            bestD = d;
            bestU = u;
            bestV = v;
            found = true;
          }
        }
      }
      if (!found) break;
      inTree[bestV] = true;
      degree[bestU]++;
      degree[bestV]++;
      edges.push_back({bestU, bestV});
    }

    // Emit crisp fine ray lines with generous breathing gaps around each star symbol!
    for (const auto& e : edges) {
      const auto& a = group[e.first];
      const auto& b = group[e.second];
      const float d = std::hypot(b.x - a.x, b.y - a.y);
      if (d < 12.0f) continue;
      const float ux = (b.x - a.x) / d;
      const float uy = (b.y - a.y) / d;
      const float gapA = (a.mag > 0.8f) ? 7.5f : 5.0f;
      const float gapB = (b.mag > 0.8f) ? 7.5f : 5.0f;
      if (d > gapA + gapB + 3.0f) {
        sg.addStroke(2, a.x + ux * gapA, a.y + uy * gapA, b.x - ux * gapB, b.y - uy * gapB, 0.52f, 0.52f, false);
      }
    }

    // Emit hierarchical star symbols (Alpha 4-point needle star, Beta ringed star, Gamma ink star).
    for (const auto& st : group) {
      if (st.mag > 0.82f && field.clearanceAt(st.x, st.y) >= 11.0f) {
        sg.addDisc(7, st.x, st.y, rng.range(7.2f, 9.5f), DiscKind::CelestialStar4, 0.0f);
      } else if (st.mag > 0.55f) {
        sg.addDisc(7, st.x, st.y, 3.6f, DiscKind::RingedStar, 0.0f);
      } else {
        sg.addDisc(7, st.x, st.y, 2.0f, DiscKind::StippledBud, 0.0f, 0, 15);
      }
    }
  }

  // Scatter delicate unconnected pinpoint field stars & 1 shooting star / comet across open sky pockets!
  const int nFieldStars = std::clamp(reg.areaPx / 2600, 10, 32);
  bool placedComet = false;
  for (int i = 0; i < nFieldStars * 4 && i < 96; ++i) {
    const float fx = rng.range(static_cast<float>(reg.x0 + 10), static_cast<float>(reg.x1 - 10));
    const float fy = rng.range(static_cast<float>(reg.y0 + 10), static_cast<float>(reg.y1 - 10));
    if (!isSkySpotValid(fx, fy, 8.0f)) continue;
    bool nearConst = false;
    for (const auto& c : constellations) {
      for (const auto& st : c) {
        if (std::hypot(fx - st.x, fy - st.y) < 16.0f) nearConst = true;
      }
    }
    if (nearConst) continue;

    if (!placedComet && field.clearanceAt(fx, fy) >= 18.0f && isSkySpotValid(fx + 28.0f, fy - 12.0f, 10.0f)) {
      // Graceful copperplate shooting comet!
      sg.addDisc(7, fx, fy, 6.2f, DiscKind::CelestialStar4, 0.22f);
      for (int t = -1; t <= 1; ++t) {
        const float ta = -0.36f + t * 0.08f;
        const float tl = (t == 0) ? 32.0f : 24.0f;
        sg.addStroke(3, fx + std::cos(ta) * 5.5f, fy + std::sin(ta) * 5.5f, fx + std::cos(ta) * tl,
                     fy + std::sin(ta) * tl, 0.58f, 0.42f, false, false, t != 0);
      }
      placedComet = true;
    } else if (rng.chance(0.45f)) {
      sg.addDisc(4, fx, fy, rng.chance(0.25f) ? 1.6f : 1.05f, DiscKind::StippledBud, 0.0f, 0, 15);
    }
  }
}

// ============================================================================
// Theme 4: William Morris / Art Nouveau Scrollwork & Filigree
// ============================================================================

void emitEulerScroll(SceneGraph& sg, const ClearanceField& field, float sx, float sy, float startAngle, float scale,
                     float turnSign, Rng& rng) {
  std::vector<std::pair<float, float>> pts;
  pts.reserve(44);
  float x = sx;
  float y = sy;
  float theta = startAngle;
  pts.push_back({x, y});

  const int steps = 46;
  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / steps;
    // Euler spiral: curvature increases smoothly with arc length into a lush 1.7-turn involute eye!
    const float dTheta = turnSign * (0.055f + 0.44f * t * t);
    const float ds = scale * 0.068f * (1.0f - 0.48f * t);
    theta += dTheta;
    const float nx = x + std::cos(theta) * ds;
    const float ny = y + std::sin(theta) * ds;
    if (field.clearanceAt(nx, ny) < 4.5f) break;
    x = nx;
    y = ny;
    pts.push_back({x, y});

    // Lobed acanthus sheaths + counter-buds hugging the outer convex side of the scroll!
    if (i == 7 || i == 15 || i == 23 || i == 31) {
      const float outAng = theta - turnSign * 0.76f;
      const float lLen = clampf(scale * (0.28f - 0.004f * i), 9.0f, 23.0f);
      if (field.clearanceAt(x + std::cos(outAng) * lLen, y + std::sin(outAng) * lLen) >= 4.0f) {
        sg.addLeaf(3, x, y, outAng, lLen, lLen * 0.42f, turnSign * 0.22f, LeafStyle::WoodcutHalfShade, true,
                   rng.nextU32());
      }
    }
  }
  emitCurvedPolyline(sg, 2, pts, 1.85f, 0.65f, true, false, rng.nextU32());
  if (pts.size() >= 24 && field.clearanceAt(x, y) >= 8.0f) {
    sg.addDisc(6, x, y, clampf(scale * 0.11f, 5.5f, 12.5f), DiscKind::MorrisRosette, theta, 85, 245, rng.nextU32());
  }
}

void buildMorrisFiligree(SceneGraph& sg, const ClearanceField& field, const DeadRegion& reg, Rng& rng) {
  if (reg.poles.empty()) return;
  const Pole& mainPole = reg.poles[0];

  if (reg.kind == RegionKind::SceneBreakCorridor) {
    const float cx = (reg.x0 + reg.x1) * 0.5f;
    const float cy = mainPole.y;
    const float rMed = clampf(mainPole.clearance * 0.76f, 7.5f, 14.0f);
    const float scrollScale = std::min(130.0f, (reg.x1 - reg.x0) * 0.34f);
    sg.addDisc(6, cx, cy, rMed, DiscKind::MorrisRosette, 0.0f, 75, 248, rng.nextU32());
    emitEulerScroll(sg, field, cx - rMed, cy, kPi - 0.22f, scrollScale, 1.0f, rng);
    emitEulerScroll(sg, field, cx + rMed, cy, 0.22f, scrollScale, -1.0f, rng);
    return;
  }

  const bool symmetric =
      (reg.kind != RegionKind::MarginalBay) && (std::fabs(mainPole.x - field.screenW * 0.5f) < field.screenW * 0.18f);
  const float cx = symmetric ? field.screenW * 0.5f : mainPole.x;
  const float cy = mainPole.y;
  const float medR = clampf(mainPole.clearance * 0.48f, 16.0f, 36.0f);

  // Central William Morris rosette medallion + cardinal acanthus fleurons.
  sg.addDisc(6, cx, cy, medR, DiscKind::MorrisRosette, 0.0f, 75, 248, rng.nextU32());
  for (int k = 0; k < 4; ++k) {
    const float a = k * kPi * 0.5f + kPi * 0.25f;
    const float lx = cx + std::cos(a) * (medR * 0.85f);
    const float ly = cy + std::sin(a) * (medR * 0.85f);
    const float lLen = medR * 0.72f;
    if (field.clearanceAt(lx + std::cos(a) * lLen, ly + std::sin(a) * lLen) >= 5.0f) {
      sg.addLeaf(3, lx, ly, a, lLen, lLen * 0.44f, 0.0f, LeafStyle::WoodcutHalfShade, true, rng.nextU32());
    }
  }

  if (symmetric) {
    const float wingScale = std::min(215.0f, (reg.x1 - reg.x0) * 0.52f);
    // Mirrored upper & lower paired Euler scrolls + floral finials!
    emitEulerScroll(sg, field, cx - medR * 0.92f, cy, kPi - 0.32f, wingScale, 1.0f, rng);
    emitEulerScroll(sg, field, cx + medR * 0.92f, cy, 0.32f, wingScale, -1.0f, rng);
    emitEulerScroll(sg, field, cx - medR * 0.78f, cy + medR * 0.32f, kPi + 0.52f, wingScale * 0.82f, -1.0f, rng);
    emitEulerScroll(sg, field, cx + medR * 0.78f, cy + medR * 0.32f, -0.52f, wingScale * 0.82f, 1.0f, rng);
    if ((reg.y1 - reg.y0) >= 165) {
      emitEulerScroll(sg, field, cx, cy - medR * 0.92f, -kPi * 0.5f - 0.42f, wingScale * 0.68f, 1.0f, rng);
      emitEulerScroll(sg, field, cx, cy - medR * 0.92f, -kPi * 0.5f + 0.42f, wingScale * 0.68f, -1.0f, rng);
      emitEulerScroll(sg, field, cx, cy + medR * 0.92f, kPi * 0.5f + 0.42f, wingScale * 0.68f, -1.0f, rng);
      emitEulerScroll(sg, field, cx, cy + medR * 0.92f, kPi * 0.5f - 0.42f, wingScale * 0.68f, 1.0f, rng);
    }
  } else {
    // Tall asymmetric marginal bay: vertical undulating S-spine with alternating Euler curls!
    const float baySpan = static_cast<float>(reg.y1 - reg.y0);
    std::vector<std::pair<float, float>> spine;
    const int nSeg = 26;
    for (int i = 0; i <= nSeg; ++i) {
      const float t = static_cast<float>(i) / nSeg;
      const float sy = lerpf(static_cast<float>(reg.y0 + 22), static_cast<float>(reg.y1 - 22), t);
      const float sx = mainPole.x + std::sin(t * kPi * 2.5f) * std::min(22.0f, mainPole.clearance * 0.35f);
      if (field.clearanceAt(sx, sy) < 7.0f) continue;
      spine.push_back({sx, sy});
      if (i > 2 && i < nSeg - 2 && i % 5 == 0) {
        const float side = (i % 2 == 0) ? 1.0f : -1.0f;
        const float curlScale = clampf(field.clearanceAt(sx, sy) * 1.65f, 34.0f, 82.0f);
        emitEulerScroll(sg, field, sx, sy, (side > 0 ? -0.35f : kPi + 0.35f), curlScale, side, rng);
      }
    }
    emitCurvedPolyline(sg, 2, spine, 1.8f, 1.1f, true, false, rng.nextU32());
  }
}

// ============================================================================
// Theme 5: Woodblock Waves & Drifting Sakura Petals (青海波・桜吹雪)
// ============================================================================

void buildWavesAndPetals(SceneGraph& sg, const ClearanceField& field, const DeadRegion& reg,
                         const std::vector<int16_t>& cellComp, Rng& rng) {
  if (reg.poles.empty()) return;
  const Pole& mainPole = reg.poles[0];

  if (reg.kind == RegionKind::SceneBreakCorridor) {
    const float cx = (reg.x0 + reg.x1) * 0.5f;
    const float cy = mainPole.y + 3.0f;
    const float rWave = clampf(mainPole.clearance * 0.85f, 9.0f, 15.0f);
    for (int i = -2; i <= 2; ++i) {
      const float wx = cx + i * (rWave * 1.45f);
      if (field.clearanceAt(wx, cy) >= rWave * 0.7f) {
        sg.addDisc(3 + (i % 2 == 0 ? 1 : 0), wx, cy, rWave, DiscKind::SeigaihaScale, 0.0f, 92, 250, rng.nextU32());
      }
    }
    emitSakuraFlowerOrPetal(sg, cx - rWave * 4.4f, mainPole.y - 1.5f, rWave * 0.65f, false, rng);
    emitSakuraFlowerOrPetal(sg, cx + rWave * 4.2f, mainPole.y - 2.0f, rWave * 0.75f, true, rng);
    emitSakuraFlowerOrPetal(sg, cx + rWave * 5.8f, mainPole.y + 1.5f, rWave * 0.58f, false, rng);
    return;
  }

  // Scalloped Seigaiha (青海波) woodblock wave rows rising from the lower portion of the dead space!
  const float scaleR = clampf(mainPole.clearance * 0.38f, 16.0f, 27.0f);
  const float colStep = scaleR * 1.70f;
  const float rowStep = scaleR * 0.52f;
  const float waterTopBase = lerpf(static_cast<float>(reg.y0), static_cast<float>(reg.y1),
                                   (reg.kind == RegionKind::MarginalBay) ? 0.50f : 0.42f);
  const float swellPhase = rng.range(0.0f, kTwoPi);

  int rowIdx = 0;
  for (float wy = waterTopBase; wy <= static_cast<float>(reg.y1) + scaleR * 0.5f; wy += rowStep, ++rowIdx) {
    // Monotonic front-to-back zLayer so lower wave crests ALWAYS overlap upper rows without wrapping!
    const uint8_t zWave = static_cast<uint8_t>(std::min(38, 2 + rowIdx));
    const float xOffset = (rowIdx % 2 == 1) ? colStep * 0.5f : 0.0f;
    for (float wx = static_cast<float>(reg.x0) + xOffset; wx <= static_cast<float>(reg.x1); wx += colStep) {
      const float swellY = std::sin(wx * 0.022f + swellPhase) * 12.0f;
      const float cy = wy + swellY;
      if (cy < waterTopBase + swellY - 2.0f) continue;
      if (field.clearanceAt(wx, cy) < scaleR * 0.62f) continue;
      sg.addDisc(zWave, wx, cy, scaleR, DiscKind::SeigaihaScale, 0.0f, 88, 252, rng.nextU32());
    }
  }

  // Drifting cherry blossoms (桜) and tumbling notched petals rising into the open sky above the waves!
  const int nPetals = std::clamp(reg.areaPx / 2600, 8, 28);
  int placed = 0;
  for (int attempt = 0; attempt < nPetals * 8 && placed < nPetals; ++attempt) {
    const float px = rng.range(static_cast<float>(reg.x0 + 14), static_cast<float>(reg.x1 - 14));
    const float py = rng.range(static_cast<float>(reg.y0 + 14), static_cast<float>(reg.y1 - 18));
    const int gx = std::clamp(static_cast<int>(px) / kCellPx, 0, field.gw - 1);
    const int gy = std::clamp(static_cast<int>(py) / kCellPx, 0, field.gh - 1);
    if (cellComp[gy * field.gw + gx] != reg.componentId) continue;
    const float c = field.clearanceAt(px, py);
    if (c < 11.0f) continue;

    // Prefer open sky above water, with a few blossoms floating directly on top of the crests.
    const float swellHere = waterTopBase + std::sin(px * 0.022f + swellPhase) * 14.0f;
    if (py > swellHere + scaleR * 0.8f && rng.chance(0.75f)) continue;

    const bool fullFlower = (placed < 4 && c >= 20.0f) || (c >= 24.0f && rng.chance(0.28f));
    const float rBlossom = fullFlower ? clampf(c * 0.42f, 12.0f, 21.0f) : clampf(c * 0.28f, 6.5f, 11.5f);
    emitSakuraFlowerOrPetal(sg, px, py, rBlossom, fullFlower, rng);
    ++placed;
  }
}

// ============================================================================
// Strip-Streamed 1:1 Pixel Rasterizer + 1-Bit Atkinson Error Diffusion
// ============================================================================

void rasterizeStrokeInStrip(const ScenePrim& p, int stripY0, int stripH, int W, uint8_t* lumBuf, uint8_t* modeBuf,
                            bool nocturne) {
  const int yStart = std::max<int>(stripY0, p.box.y0);
  const int yEnd = std::min<int>(stripY0 + stripH - 1, p.box.y1);
  if (yStart > yEnd) return;
  const int xStart = std::max<int>(0, p.box.x0);
  const int xEnd = std::min<int>(W - 1, p.box.x1);
  if (xStart > xEnd) return;

  const float dx = p.x1 - p.x0;
  const float dy = p.y1 - p.y0;
  const float len2 = dx * dx + dy * dy;
  const float segLen = std::sqrt(len2);
  const bool whiteHalo = (p.flags & 1u) != 0;
  const bool dryBrush = (p.flags & 2u) != 0;
  const bool dotted = (p.flags & 4u) != 0;

  for (int y = yStart; y <= yEnd; ++y) {
    const int rowOff = (y - stripY0) * W;
    const float fy = static_cast<float>(y);
    for (int x = xStart; x <= xEnd; ++x) {
      const float fx = static_cast<float>(x);
      float t = 0.0f;
      if (len2 > 1e-4f) {
        t = clampf(((fx - p.x0) * dx + (fy - p.y0) * dy) / len2, 0.0f, 1.0f);
      }
      if (dotted && (static_cast<int>(t * segLen) % 5 >= 2)) continue;

      const float projX = p.x0 + t * dx;
      const float projY = p.y0 + t * dy;
      const float rHere = lerpf(p.r0, p.r1, t);
      // Most bbox pixels lie outside the stroke and its halo: skip them before
      // the square root (soft float on the ESP32-C3).
      const float ex = fx - projX;
      const float ey = fy - projY;
      const float dist2 = ex * ex + ey * ey;
      const float reach = whiteHalo ? rHere + 1.45f : rHere;
      if (dist2 > reach * reach) continue;
      const float dist = std::sqrt(dist2);

      if (!nocturne) {
        if (dist <= rHere) {
          if (dryBrush && rHere >= 1.9f && dist < rHere - 1.05f) {
            // Calligraphic feibai (飞白) bark grain inside thick plum trunks!
            const float grain = valueNoise2D(fx * 0.45f, fy * 0.45f, p.seed);
            if (grain > 0.74f) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;
              continue;
            }
          }
          lumBuf[rowOff + x] = 0;
          modeBuf[rowOff + x] = PM_FORCE_BLACK;
        } else if (whiteHalo && dist <= rHere + 1.45f) {
          if (modeBuf[rowOff + x] != PM_FORCE_BLACK) {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;
          }
        }
      } else {
        // Nocturne: crisp moonlit white stems/boughs with velvet-black crossing halos & carved feibai bark!
        if (dist <= rHere) {
          if (dryBrush && rHere >= 2.1f && dist < rHere - 1.15f) {
            const float grain = valueNoise2D(fx * 0.45f, fy * 0.45f, p.seed);
            if (grain > 0.66f) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
              continue;
            } else if (grain > 0.36f) {
              lumBuf[rowOff + x] = static_cast<uint8_t>(lerpf(225.0f, 95.0f, (grain - 0.36f) / 0.30f));
              modeBuf[rowOff + x] = PM_ATKINSON;
              continue;
            }
          }
          lumBuf[rowOff + x] = 255;
          modeBuf[rowOff + x] = PM_FORCE_WHITE;
        } else if (whiteHalo && dist <= rHere + 1.45f) {
          if (modeBuf[rowOff + x] != PM_FORCE_WHITE) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
          }
        }
      }
    }
  }
}

void rasterizePetalInStrip(const ScenePrim& p, int stripY0, int stripH, int W, uint8_t* lumBuf, uint8_t* modeBuf,
                           bool nocturne) {
  const int yStart = std::max<int>(stripY0, p.box.y0);
  const int yEnd = std::min<int>(stripY0 + stripH - 1, p.box.y1);
  if (yStart > yEnd) return;
  const int xStart = std::max<int>(0, p.box.x0);
  const int xEnd = std::min<int>(W - 1, p.box.x1);
  if (xStart > xEnd) return;

  const float ca = std::cos(p.angle);
  const float sa = std::sin(p.angle);
  const auto shape = static_cast<PetalShape>(p.subKind);

  for (int y = yStart; y <= yEnd; ++y) {
    const int rowOff = (y - stripY0) * W;
    const float ry = static_cast<float>(y) - p.y0;
    for (int x = xStart; x <= xEnd; ++x) {
      const float rx = static_cast<float>(x) - p.x0;
      const float along = rx * ca + ry * sa;
      const float perp = -rx * sa + ry * ca;

      const float u = along / p.length;
      if (u < -0.08f || u > 1.14f) continue;

      const float uc = clampf(u, 0.0f, 1.0f);
      float profile = 0.0f;
      float tipLimit = 1.0f;
      switch (shape) {
        case PetalShape::WildRoseNotched: {
          profile = std::sin(std::pow(uc, 0.72f) * kPi) * (0.66f + 0.52f * uc);
          const float vNormEst = std::fabs(perp) / std::max(1.0f, p.width * 0.5f);
          tipLimit = 1.0f - 0.11f * std::exp(-vNormEst * vNormEst * 9.0f);
          break;
        }
        case PetalShape::PeonyRuffled: {
          profile = std::sin(std::pow(uc, 0.65f) * kPi) * (0.72f + 0.42f * uc);
          const float vAng = perp / std::max(1.0f, p.width * 0.5f);
          tipLimit = 0.96f + 0.055f * std::cos(vAng * 7.5f + (p.seed & 7));
          break;
        }
        case PetalShape::CosmosFluted: {
          profile = std::sin(std::pow(uc, 0.78f) * kPi) * 0.92f;
          const float vAng = perp / std::max(1.0f, p.width * 0.5f);
          tipLimit = 0.97f + 0.035f * std::cos(vAng * 6.0f);
          break;
        }
        case PetalShape::PlumRound: {
          const float du = (u - 0.54f) / 0.54f;
          profile = (std::fabs(du) < 1.0f) ? std::sqrt(std::max(0.0f, 1.0f - du * du)) : 0.0f;
          tipLimit = 1.04f;
          break;
        }
        case PetalShape::SakuraCleft: {
          profile = std::sin(std::pow(uc, 0.74f) * kPi) * (0.65f + 0.48f * uc);
          const float vNormEst = std::fabs(perp) / std::max(1.0f, p.width * 0.5f);
          tipLimit = 1.02f - 0.22f * std::max(0.0f, 1.0f - vNormEst * 3.2f);
          break;
        }
        case PetalShape::BellCorolla: {
          // Side-view hanging bellflower: rounded dome neck swelling and flaring at scalloped bell rim!
          const float dome = std::sqrt(std::max(0.0f, clampf(uc * 3.2f, 0.0f, 1.0f)));
          const float flare = std::pow(clampf((uc - 0.55f) / 0.45f, 0.0f, 1.0f), 2.0f) * 0.34f;
          profile = dome * (0.58f + 0.14f * uc) + flare;
          const float vRelEst = clampf(perp / std::max(1.0f, p.width * 0.5f), -1.0f, 1.0f);
          tipLimit = 0.94f + 0.075f * std::cos(vRelEst * kPi * 2.5f);  // 3 scalloped bell lobes!
          break;
        }
        case PetalShape::TrumpetCorolla: {
          // Side-profile nectar trumpet: slender tube expanding into a wide fluted trumpet mouth!
          const float tube = 0.18f + 0.14f * uc;
          const float horn = std::pow(clampf((uc - 0.32f) / 0.68f, 0.0f, 1.0f), 2.1f) * 0.78f;
          profile = std::sqrt(clampf(uc * 8.0f, 0.0f, 1.0f)) * (tube + horn);
          const float vRelEst = clampf(perp / std::max(1.0f, p.width * 0.5f), -1.0f, 1.0f);
          tipLimit = 0.95f + 0.06f * std::cos(vRelEst * kPi * 3.0f);
          break;
        }
        case PetalShape::DaisyOblong: {
          // Smooth strap-shaped ray petal with rounded semicircular tip!
          profile = std::sin(std::pow(uc, 0.52f) * kPi) * 0.96f;
          tipLimit = 1.0f;
          break;
        }
      }

      const float halfW = p.width * 0.5f * profile;
      const float dSide = std::fabs(perp) - halfW;
      const float dTip = (u - tipLimit) * p.length;
      const float dBase = -u * p.length;
      const float sdf = std::max({dSide, dTip, dBase});

      if (sdf > (nocturne ? 1.78f : 1.55f)) continue;

      if (!nocturne) {
        if (sdf > 0.0f) {
          lumBuf[rowOff + x] = 255;
          modeBuf[rowOff + x] = PM_FORCE_WHITE;
        } else if (sdf >= -1.18f) {
          lumBuf[rowOff + x] = 0;
          modeBuf[rowOff + x] = PM_FORCE_BLACK;
        } else if (shape == PetalShape::BellCorolla || shape == PetalShape::TrumpetCorolla) {
          // Side-profile bell/trumpet: crisp longitudinal corolla lobe creases + deep shaded throat!
          const float vRel = perp / std::max(1.5f, halfW);
          const bool lobeSeam = (uc > 0.25f && uc < 0.92f) && (std::fabs(std::fabs(vRel) - 0.38f) < 0.055f);
          if (lobeSeam) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
          } else if (sdf >= -2.2f) {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;
          } else {
            float lum = lerpf(static_cast<float>(p.lumA), static_cast<float>(p.lumB), uc);
            // Darker cylindrical side-wall shading + dark nectar opening near mouth of trumpet!
            lum -= 42.0f * (vRel * vRel);
            if (shape == PetalShape::TrumpetCorolla && uc > 0.78f && std::fabs(vRel) < 0.55f) {
              lum -= 65.0f * (1.0f - std::fabs(vRel) / 0.55f);
            }
            lumBuf[rowOff + x] = static_cast<uint8_t>(clampf(lum, 28.0f, 255.0f));
            modeBuf[rowOff + x] = PM_ATKINSON;
          }
        } else if (sdf >= -2.35f) {
          lumBuf[rowOff + x] = 255;
          modeBuf[rowOff + x] = PM_FORCE_WHITE;
        } else {
          const float vRel = perp / std::max(1.5f, halfW);
          float tCup = std::pow(clampf(u / std::max(0.2f, tipLimit), 0.0f, 1.0f), 0.92f);
          float lum = lerpf(static_cast<float>(p.lumA), static_cast<float>(p.lumB), tCup);
          lum += 10.0f * (vRel * vRel);

          if (u > 0.08f && u < 0.74f) {
            const float veinWave = std::fabs(std::sin(vRel * (shape == PetalShape::CosmosFluted ? 4.8f : 6.2f)));
            if (veinWave < 0.15f) {
              lum -= (1.0f - u / 0.74f) * 62.0f;
            }
          }
          lumBuf[rowOff + x] = static_cast<uint8_t>(clampf(lum, 24.0f, 255.0f));
          modeBuf[rowOff + x] = PM_ATKINSON;
        }
      } else {
        // Nocturne moonlit petal: 1.78px velvet-black separation moat between overlapping petals,
        // fine 1.18px white petal contour, and delicate silver cupping toward a shadowed flower throat!
        if (sdf > 0.0f) {
          lumBuf[rowOff + x] = 0;
          modeBuf[rowOff + x] = PM_FORCE_BLACK;
        } else if (shape == PetalShape::BellCorolla || shape == PetalShape::TrumpetCorolla) {
          const float vRel = perp / std::max(1.5f, halfW);
          const bool lobeSeam = (uc > 0.25f && uc < 0.92f) && (std::fabs(std::fabs(vRel) - 0.38f) < 0.055f);
          if (sdf >= -1.25f) {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;
          } else if (lobeSeam) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
          } else {
            float lum = lerpf(static_cast<float>(p.lumA), static_cast<float>(p.lumB), uc);
            lum -= 42.0f * (vRel * vRel);
            if (shape == PetalShape::TrumpetCorolla && uc > 0.78f && std::fabs(vRel) < 0.55f) {
              lum -= 75.0f * (1.0f - std::fabs(vRel) / 0.55f);
            }
            lumBuf[rowOff + x] = static_cast<uint8_t>(clampf(lum, 20.0f, 255.0f));
            modeBuf[rowOff + x] = PM_ATKINSON;
          }
        } else if (sdf >= -1.18f) {
          lumBuf[rowOff + x] = 255;
          modeBuf[rowOff + x] = PM_FORCE_WHITE;
        } else {
          const float vRel = perp / std::max(1.5f, halfW);
          const float throatLum = std::min<float>(static_cast<float>(p.lumA) * 0.60f, 76.0f);
          const float tipLum = std::min<float>(static_cast<float>(p.lumB), 240.0f);
          float tCup = std::pow(clampf(u / std::max(0.2f, tipLimit), 0.0f, 1.0f), 1.12f);
          float lum = lerpf(throatLum, tipLum, tCup);
          lum += 14.0f * (vRel * vRel);

          if (u > 0.08f && u < 0.74f) {
            const float veinWave = std::fabs(std::sin(vRel * (shape == PetalShape::CosmosFluted ? 4.8f : 6.2f)));
            if (veinWave < 0.15f) {
              lum -= (1.0f - u / 0.74f) * 72.0f;
            }
          }
          lumBuf[rowOff + x] = static_cast<uint8_t>(clampf(lum, 18.0f, 255.0f));
          modeBuf[rowOff + x] = PM_ATKINSON;
        }
      }
    }
  }
}

void rasterizeLeafInStrip(const ScenePrim& p, int stripY0, int stripH, int W, uint8_t* lumBuf, uint8_t* modeBuf,
                          bool nocturne) {
  const int yStart = std::max<int>(stripY0, p.box.y0);
  const int yEnd = std::min<int>(stripY0 + stripH - 1, p.box.y1);
  if (yStart > yEnd) return;
  const int xStart = std::max<int>(0, p.box.x0);
  const int xEnd = std::min<int>(W - 1, p.box.x1);
  if (xStart > xEnd) return;

  const float ca = std::cos(p.angle);
  const float sa = std::sin(p.angle);
  const auto style = static_cast<LeafStyle>(p.subKind);
  const bool serrated = (p.flags & 2u) != 0;

  for (int y = yStart; y <= yEnd; ++y) {
    const int rowOff = (y - stripY0) * W;
    const float ry = static_cast<float>(y) - p.y0;
    for (int x = xStart; x <= xEnd; ++x) {
      const float rx = static_cast<float>(x) - p.x0;
      const float along = rx * ca + ry * sa;
      const float u = along / p.length;
      if (u < -0.18f || u > 1.06f) continue;

      const float uc = clampf(u, 0.0f, 1.0f);
      const float spineOffset = p.curvature * p.length * uc * uc;
      const float perp = (-rx * sa + ry * ca) - spineOffset;

      float env = 0.0f;
      float baseCut = 0.0f;
      if (style == LeafStyle::HeartCordate) {
        // Broad heart-shaped ivy / morning-glory leaf with twin basal auricle lobes!
        const float uShift = clampf((u + 0.14f) / 1.14f, 0.0f, 1.0f);
        env = std::sin(std::pow(uShift, 0.52f) * kPi) * (1.08f - 0.36f * uShift);
        const float vAbs = std::fabs(perp) / std::max(1.0f, p.width * 0.5f);
        baseCut = -0.13f * std::sin(clampf(vAbs * 1.4f, 0.0f, 1.0f) * kPi);  // Cordate basal notch!
      } else if (style == LeafStyle::ScallopedOak) {
        // Multi-lobed scalloped oak / acanthus leaf!
        env = std::sin(std::pow(uc, 0.62f) * kPi) * (0.76f + 0.28f * std::fabs(std::cos(uc * kPi * 3.5f)));
      } else {
        // Lanceolate / ovate leaf blade envelope with pointed acuminate tip.
        env = std::sin(std::pow(uc, 0.68f) * kPi) * (1.0f - 0.25f * uc);
        if (serrated && uc > 0.16f && uc < 0.92f) {
          env *= (1.0f + 0.085f * std::sin(uc * 34.0f));
        }
      }

      const float halfW = p.width * 0.5f * env;
      const float dSide = std::fabs(perp) - halfW;
      const float dTip = (u - 1.0f) * p.length;
      const float dBase = (baseCut - u) * p.length;
      const float sdf = std::max({dSide, dTip, dBase});

      if (sdf > 1.35f) continue;

      if (!nocturne) {
        if (sdf > 0.0f) {
          if (modeBuf[rowOff + x] != PM_FORCE_BLACK) {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;
          }
          continue;
        }

        if (style == LeafStyle::SolidInkBamboo) {
          if (p.width >= 6.5f && std::fabs(perp) < 0.55f && uc > 0.12f && uc < 0.72f) {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;
          } else {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
          }
        } else if (style == LeafStyle::InkWashBamboo) {
          lumBuf[rowOff + x] = static_cast<uint8_t>(lerpf(118.0f, 175.0f, uc));
          modeBuf[rowOff + x] = PM_ATKINSON;
        } else {
          // WoodcutHalfShade / HeartCordate / ScallopedOak: crisp ink margin + midrib + bilateral woodcut shading!
          if (sdf >= -1.12f || (std::fabs(perp) <= 0.68f && uc > 0.0f && uc < 0.88f)) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
          } else if (perp > 0.0f) {
            const float diagVein = std::sin((uc * p.length - perp * 0.85f) * 1.15f);
            float lum = lerpf(118.0f, 205.0f, uc * 0.65f + (perp / std::max(1.0f, halfW)) * 0.35f);
            if (diagVein > 0.78f) lum -= 52.0f;
            lumBuf[rowOff + x] = static_cast<uint8_t>(clampf(lum, 30.0f, 245.0f));
            modeBuf[rowOff + x] = PM_ATKINSON;
          } else {
            const float rim = clampf(-sdf / std::max(1.5f, halfW), 0.0f, 1.0f);
            lumBuf[rowOff + x] = (rim < 0.24f) ? 212 : 255;
            modeBuf[rowOff + x] = (rim < 0.24f) ? PM_ATKINSON : PM_FORCE_WHITE;
          }
        }
      } else {
        // Nocturne leaf: preserve stem connection at u <= 0.08, carve black moat along blade/sepals,
        // and shade leaves with white outlines/midribs + half-silver/half-velvet-black woodcut relief
        // so foliage never overpowers the brighter blossoms!
        if (sdf > 0.0f) {
          if (u > 0.08f || modeBuf[rowOff + x] != PM_FORCE_WHITE) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
          }
          continue;
        }

        if (style == LeafStyle::SolidInkBamboo) {
          if (p.width >= 6.5f && std::fabs(perp) < 0.55f && uc > 0.12f && uc < 0.72f) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
          } else {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;
          }
        } else if (style == LeafStyle::InkWashBamboo) {
          lumBuf[rowOff + x] = static_cast<uint8_t>(lerpf(108.0f, 62.0f, uc));
          modeBuf[rowOff + x] = PM_ATKINSON;
        } else {
          if (sdf >= -1.12f || (std::fabs(perp) <= 0.68f && uc > 0.0f && uc < 0.88f)) {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;
          } else if (perp > 0.0f) {
            const float diagVein = std::sin((uc * p.length - perp * 0.85f) * 1.15f);
            float lum = lerpf(64.0f, 146.0f, uc * 0.55f + (perp / std::max(1.0f, halfW)) * 0.45f);
            if (diagVein > 0.78f) lum += 72.0f;
            lumBuf[rowOff + x] = static_cast<uint8_t>(clampf(lum, 24.0f, 242.0f));
            modeBuf[rowOff + x] = PM_ATKINSON;
          } else {
            const float rim = clampf(-sdf / std::max(1.5f, halfW), 0.0f, 1.0f);
            lumBuf[rowOff + x] = (rim < 0.22f) ? 58 : 0;
            modeBuf[rowOff + x] = (rim < 0.22f) ? PM_ATKINSON : PM_FORCE_BLACK;
          }
        }
      }
    }
  }
}

void rasterizeDiscInStrip(const ScenePrim& p, int stripY0, int stripH, int W, uint8_t* lumBuf, uint8_t* modeBuf,
                          bool nocturne) {
  const int yStart = std::max<int>(stripY0, p.box.y0);
  const int yEnd = std::min<int>(stripY0 + stripH - 1, p.box.y1);
  if (yStart > yEnd) return;
  const int xStart = std::max<int>(0, p.box.x0);
  const int xEnd = std::min<int>(W - 1, p.box.x1);
  if (xStart > xEnd) return;

  const auto kind = static_cast<DiscKind>(p.subKind);

  if (kind == DiscKind::StamenCrown || kind == DiscKind::PlumCalyxPins) {
    const float coreR = p.r0 * (kind == DiscKind::PlumCalyxPins ? 0.36f : 0.56f);
    for (int y = yStart; y <= yEnd; ++y) {
      const int rowOff = (y - stripY0) * W;
      for (int x = xStart; x <= xEnd; ++x) {
        const float d = std::hypot(static_cast<float>(x) - p.x0, static_cast<float>(y) - p.y0);
        if (!nocturne) {
          if (d <= coreR + 1.3f) {
            if (d > coreR) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
            } else {
              lumBuf[rowOff + x] = (kind == DiscKind::PlumCalyxPins) ? 45 : 135;
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          }
        } else {
          // Nocturne: velvet-black throat moat so white stamen filaments & pins glow crisply!
          if (d <= p.r0 * 0.88f) {
            if (d <= coreR) {
              lumBuf[rowOff + x] = (kind == DiscKind::PlumCalyxPins) ? 32 : 98;
              modeBuf[rowOff + x] = PM_ATKINSON;
            } else if (d <= coreR + 1.15f) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;
            } else {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
            }
          }
        }
      }
    }
    const int nPins = (kind == DiscKind::PlumCalyxPins) ? 9 : 16;
    for (int i = 0; i < nPins; ++i) {
      const float a = p.angle + (kind == DiscKind::PlumCalyxPins ? (kTwoPi * i) / nPins : i * kGoldenAngle);
      const float rPin = p.r0 * ((i % 2 == 0) ? 1.0f : 0.82f);
      const float ax = p.x0 + std::cos(a) * rPin;
      const float ay = p.y0 + std::sin(a) * rPin;
      ScenePrim ray;
      ray.type = PrimType::Stroke;
      ray.x0 = p.x0 + std::cos(a) * coreR;
      ray.y0 = p.y0 + std::sin(a) * coreR;
      ray.x1 = ax;
      ray.y1 = ay;
      ray.r0 = 0.48f;
      ray.r1 = 0.48f;
      ray.box = p.box;
      rasterizeStrokeInStrip(ray, stripY0, stripH, W, lumBuf, modeBuf, nocturne);

      for (int y = std::max<int>(yStart, static_cast<int>(ay - 2)); y <= std::min<int>(yEnd, static_cast<int>(ay + 2));
           ++y) {
        for (int x = std::max<int>(xStart, static_cast<int>(ax - 2));
             x <= std::min<int>(xEnd, static_cast<int>(ax + 2)); ++x) {
          const float dPin = std::hypot(static_cast<float>(x) - ax, static_cast<float>(y) - ay);
          if (dPin <= 1.35f) {
            lumBuf[(y - stripY0) * W + x] = nocturne ? 255 : 0;
            modeBuf[(y - stripY0) * W + x] = nocturne ? PM_FORCE_WHITE : PM_FORCE_BLACK;
          } else if (nocturne && dPin <= 2.25f && modeBuf[(y - stripY0) * W + x] != PM_FORCE_WHITE) {
            lumBuf[(y - stripY0) * W + x] = 0;
            modeBuf[(y - stripY0) * W + x] = PM_FORCE_BLACK;
          }
        }
      }
    }
    return;
  }

  if (kind == DiscKind::FacetedCompassStar || kind == DiscKind::CelestialStar4) {
    const bool needle = (kind == DiscKind::CelestialStar4);
    for (int y = yStart; y <= yEnd; ++y) {
      const int rowOff = (y - stripY0) * W;
      const float dy = static_cast<float>(y) - p.y0;
      for (int x = xStart; x <= xEnd; ++x) {
        const float dx = static_cast<float>(x) - p.x0;
        const float r = std::hypot(dx, dy);
        if (r > p.r0 + 1.6f) continue;
        const float theta = std::atan2(dy, dx) - p.angle;
        float starProfile = 0.0f;
        if (needle) {
          // Slender copperplate celestial needle star: 4 long primary rays + 4 short diagonal glints!
          const float mainRay = std::pow(std::fabs(std::cos(2.0f * theta)), 6.0f);
          const float diagRay = 0.42f * std::pow(std::fabs(std::sin(2.0f * theta)), 8.0f);
          starProfile = p.r0 * (0.14f + 0.86f * std::max(mainRay, diagRay));
        } else {
          starProfile = p.r0 * (0.24f + 0.76f * std::pow(std::fabs(std::cos(2.0f * theta)), 2.6f));
        }
        if (!nocturne) {
          if (r <= starProfile) {
            if (needle) {
              if (r < p.r0 * 0.16f) {
                lumBuf[rowOff + x] = 255;
                modeBuf[rowOff + x] = PM_FORCE_WHITE;
              } else {
                lumBuf[rowOff + x] = 0;
                modeBuf[rowOff + x] = PM_FORCE_BLACK;
              }
            } else {
              const int octant = static_cast<int>(std::floor(((theta + kTwoPi) / (kPi * 0.25f)))) & 1;
              if (r >= starProfile - 0.95f || octant == 0) {
                lumBuf[rowOff + x] = 0;
                modeBuf[rowOff + x] = PM_FORCE_BLACK;
              } else {
                lumBuf[rowOff + x] = 255;
                modeBuf[rowOff + x] = PM_FORCE_WHITE;
              }
            }
          } else if (r <= starProfile + 1.35f && modeBuf[rowOff + x] != PM_FORCE_BLACK) {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;
          }
        } else {
          if (r <= starProfile) {
            if (needle) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;
            } else {
              const int octant = static_cast<int>(std::floor(((theta + kTwoPi) / (kPi * 0.25f)))) & 1;
              if (r >= starProfile - 0.95f || octant == 0) {
                lumBuf[rowOff + x] = 255;
                modeBuf[rowOff + x] = PM_FORCE_WHITE;
              } else {
                lumBuf[rowOff + x] = 0;
                modeBuf[rowOff + x] = PM_FORCE_BLACK;
              }
            }
          } else if (r <= starProfile + 1.35f && modeBuf[rowOff + x] != PM_FORCE_WHITE) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
          }
        }
      }
    }
    return;
  }

  if (kind == DiscKind::RingedStar) {
    // 2nd-magnitude Uranometria bullseye star: solid core + moat + fine outer ring!
    for (int y = yStart; y <= yEnd; ++y) {
      const int rowOff = (y - stripY0) * W;
      for (int x = xStart; x <= xEnd; ++x) {
        const float r = std::hypot(static_cast<float>(x) - p.x0, static_cast<float>(y) - p.y0);
        if (r > p.r0 + 1.3f) continue;
        if (!nocturne) {
          if (r > p.r0) {
            if (modeBuf[rowOff + x] != PM_FORCE_BLACK) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;
            }
          } else if (r >= p.r0 - 0.85f || r <= p.r0 * 0.38f) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
          } else {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;
          }
        } else {
          if (r > p.r0) {
            if (modeBuf[rowOff + x] != PM_FORCE_WHITE) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
            }
          } else if (r >= p.r0 - 0.85f || r <= p.r0 * 0.42f) {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;
          } else {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
          }
        }
      }
    }
    return;
  }

  if (kind == DiscKind::RingedPlanet) {
    // Miniature Saturn: tilted elliptical ring + 3D shaded planet globe!
    const float ca = std::cos(p.angle);
    const float sa = std::sin(p.angle);
    for (int y = yStart; y <= yEnd; ++y) {
      const int rowOff = (y - stripY0) * W;
      const float dy = static_cast<float>(y) - p.y0;
      for (int x = xStart; x <= xEnd; ++x) {
        const float dx = static_cast<float>(x) - p.x0;
        const float rx = dx * ca + dy * sa;
        const float ry = -dx * sa + dy * ca;
        const float dist = std::hypot(dx, dy);
        const float ellR = std::hypot(rx / 2.15f, ry / 0.58f);
        const bool inRing = (ellR >= p.r0 * 0.68f && ellR <= p.r0 * 1.02f);
        const bool frontRing = inRing && (ry >= 0.0f);

        if (!nocturne) {
          if (frontRing || (inRing && dist > p.r0 + 1.0f)) {
            if (std::fabs(ellR - p.r0 * 0.85f) < p.r0 * 0.06f) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;  // Cassini division highlight!
            } else {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
            }
          } else if (dist <= p.r0) {
            if (dist >= p.r0 - 1.0f) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
            } else {
              const float lit = clampf(0.55f - 0.45f * (dx / p.r0) - 0.35f * (dy / p.r0), 0.08f, 1.0f);
              lumBuf[rowOff + x] = static_cast<uint8_t>(lerpf(45.0f, 248.0f, lit));
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          } else if (dist <= p.r0 + 1.3f && modeBuf[rowOff + x] != PM_FORCE_BLACK) {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;
          }
        } else {
          // Nocturne Saturn: glowing white rings split by a dark Cassini division over a 3D sunlit planet!
          const float lit = clampf(0.55f - 0.45f * (dx / p.r0) - 0.35f * (dy / p.r0), 0.08f, 1.0f);
          if (frontRing || (inRing && dist > p.r0 + 1.0f)) {
            if (std::fabs(ellR - p.r0 * 0.85f) < p.r0 * 0.06f) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
            } else {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;
            }
          } else if (dist <= p.r0) {
            if (dist >= p.r0 - 1.0f) {
              lumBuf[rowOff + x] = (lit > 0.32f) ? 255 : 112;
              modeBuf[rowOff + x] = (lit > 0.32f) ? PM_FORCE_WHITE : PM_ATKINSON;
            } else {
              lumBuf[rowOff + x] = static_cast<uint8_t>(lerpf(26.0f, 250.0f, lit));
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          } else if (dist <= p.r0 + 1.3f) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
          }
        }
      }
    }
    return;
  }

  const float lx = std::sin(p.angle);
  const float ly = -0.16f;
  const float lz = std::cos(p.angle);
  const float lNorm = std::sqrt(lx * lx + ly * ly + lz * lz);

  for (int y = yStart; y <= yEnd; ++y) {
    const int rowOff = (y - stripY0) * W;
    const float dy = static_cast<float>(y) - p.y0;
    for (int x = xStart; x <= xEnd; ++x) {
      const float dx = static_cast<float>(x) - p.x0;
      const float dist = std::hypot(dx, dy);

      if (kind == DiscKind::SeigaihaScale) {
        // Restrict each wave scale to its true upper semicircular arch (dy <= 0.06 * r0)
        // so card-adjacent or bottom-row waves NEVER expose a circular bullseye bottom!
        if (dy > p.r0 * 0.06f || dist > p.r0 + 1.4f) continue;
        if (!nocturne) {
          if (dist > p.r0) {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;
            continue;
          }
          const float uRad = dist / p.r0;
          const float ringWave = std::fabs(std::sin(uRad * kPi * 4.0f));
          if (dist >= p.r0 - 1.15f || (ringWave < 0.14f && uRad > 0.18f)) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
          } else if (uRad > 0.88f) {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;
          } else {
            const float lum = lerpf(245.0f, static_cast<float>(p.lumA), std::pow(uRad, 1.35f));
            lumBuf[rowOff + x] = static_cast<uint8_t>(clampf(lum, 55.0f, 252.0f));
            modeBuf[rowOff + x] = PM_ATKINSON;
          }
        } else {
          // Nocturne Seigaiha: black separation moat, crisp white foam crest & concentric silver arcs over dark sea!
          if (dist > p.r0) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
            continue;
          }
          const float uRad = dist / p.r0;
          const float ringWave = std::fabs(std::sin(uRad * kPi * 3.5f));
          if (dist >= p.r0 - 1.2f || (ringWave < 0.125f && uRad > 0.24f)) {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;
          } else if (uRad > 0.85f) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
          } else {
            const float lum = lerpf(10.0f, 132.0f, std::pow(uRad, 1.85f));
            lumBuf[rowOff + x] = static_cast<uint8_t>(clampf(lum, 0.0f, 175.0f));
            modeBuf[rowOff + x] = PM_ATKINSON;
          }
        }
        continue;
      }

      if (dist > p.r0 + 1.6f) continue;

      // Pinpoint stars & stamen tips (StippledBud with p.lumB <= 30) in Nocturne: crisp glowing white dots!
      if (nocturne && kind == DiscKind::StippledBud && p.lumB <= 30) {
        if (dist <= p.r0) {
          lumBuf[rowOff + x] = 255;
          modeBuf[rowOff + x] = PM_FORCE_WHITE;
        } else if (dist <= p.r0 + 1.25f && modeBuf[rowOff + x] != PM_FORCE_WHITE) {
          lumBuf[rowOff + x] = 0;
          modeBuf[rowOff + x] = PM_FORCE_BLACK;
        }
        continue;
      }

      if (dist > p.r0) {
        lumBuf[rowOff + x] = nocturne ? 0 : 255;
        modeBuf[rowOff + x] = nocturne ? PM_FORCE_BLACK : PM_FORCE_WHITE;
        continue;
      }
      if (dist >= p.r0 - 1.15f) {
        if (!nocturne) {
          lumBuf[rowOff + x] = 0;
          modeBuf[rowOff + x] = PM_FORCE_BLACK;
        } else if (kind == DiscKind::Moon3D) {
          const float nx = dx / p.r0;
          const float ny = dy / p.r0;
          const float ndotl = (nx * lx + ny * ly) / lNorm;
          lumBuf[rowOff + x] = (ndotl > -0.05f) ? 255 : 108;
          modeBuf[rowOff + x] = (ndotl > -0.05f) ? PM_FORCE_WHITE : PM_ATKINSON;
        } else {
          lumBuf[rowOff + x] = 255;
          modeBuf[rowOff + x] = PM_FORCE_WHITE;
        }
        continue;
      }

      if (kind == DiscKind::SeedheadDome) {
        // Convex cross-hatched sunflower / daisy seedhead dome!
        const float hatchA = std::fabs(std::sin((dx + dy) * 0.88f));
        const float hatchB = std::fabs(std::sin((dx - dy) * 0.88f));
        if (hatchA < 0.22f || hatchB < 0.22f) {
          lumBuf[rowOff + x] = 0;
          modeBuf[rowOff + x] = PM_FORCE_BLACK;
        } else {
          const float domeLit = std::sqrt(std::max(0.0f, 1.0f - (dist * dist) / (p.r0 * p.r0)));
          lumBuf[rowOff + x] = static_cast<uint8_t>(lerpf(nocturne ? 78.0f : 95.0f, 245.0f, domeLit));
          modeBuf[rowOff + x] = PM_ATKINSON;
        }
      } else if (kind == DiscKind::Moon3D) {
        const float nx = dx / p.r0;
        const float ny = dy / p.r0;
        const float nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
        const float ndotl = (nx * lx + ny * ly + nz * lz) / lNorm;
        const float maria = fbm2D((nx + 1.5f) * 3.2f, (ny + 1.5f) * 3.2f, p.seed);
        float lum = 0.0f;
        if (ndotl <= -0.06f) {
          lum = nocturne ? (10.0f + 16.0f * (1.0f - nz)) : (22.0f + 26.0f * (1.0f - nz));
        } else {
          const float lit = smoothstepf(-0.06f, 0.82f, ndotl);
          lum = lerpf(28.0f, 255.0f, lit) - (1.0f - maria) * 48.0f * lit;
        }
        lumBuf[rowOff + x] = static_cast<uint8_t>(clampf(lum, nocturne ? 6.0f : 12.0f, 255.0f));
        modeBuf[rowOff + x] = PM_ATKINSON;
      } else if (kind == DiscKind::MorrisRosette) {
        const float uRad = dist / p.r0;
        const float theta = std::atan2(dy, dx) - p.angle;
        const float petalStar = 0.52f + 0.24f * std::cos(8.0f * theta);
        if (std::fabs(uRad - 0.78f) < 0.06f || std::fabs(uRad - 0.24f) < 0.07f) {
          lumBuf[rowOff + x] = nocturne ? 255 : 0;
          modeBuf[rowOff + x] = nocturne ? PM_FORCE_WHITE : PM_FORCE_BLACK;
        } else if (uRad < petalStar && uRad > 0.28f) {
          lumBuf[rowOff + x] = 255;
          modeBuf[rowOff + x] = PM_FORCE_WHITE;
        } else {
          lumBuf[rowOff + x] = nocturne ? 82 : 128;
          modeBuf[rowOff + x] = PM_ATKINSON;
        }
      } else {
        if (p.lumB <= 30) {
          lumBuf[rowOff + x] = 0;
          modeBuf[rowOff + x] = PM_FORCE_BLACK;
        } else {
          const float spec = std::hypot(dx / p.r0 + 0.32f, dy / p.r0 + 0.32f);
          const float minBud = nocturne ? std::min(88.0f, static_cast<float>(p.lumA)) : static_cast<float>(p.lumA);
          const float lum = lerpf(255.0f, minBud, clampf(spec * 0.78f, 0.0f, 1.0f));
          lumBuf[rowOff + x] = static_cast<uint8_t>(lum);
          modeBuf[rowOff + x] = PM_ATKINSON;
        }
      }
    }
  }
}

// Smooth polynomial union + tapered 2D capsule distance helpers for woodcut Garden Fauna!
inline float smin2D(float a, float b, float k) {
  const float h = clampf(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
  return lerpf(b, a, h) - k * h * (1.0f - h);
}

inline float sdCapsuleTapered(float px, float py, float ax, float ay, float bx, float by, float ra, float rb) {
  const float vx = bx - ax;
  const float vy = by - ay;
  const float len2 = vx * vx + vy * vy;
  const float t = (len2 > 1e-5f) ? clampf(((px - ax) * vx + (py - ay) * vy) / len2, 0.0f, 1.0f) : 0.0f;
  return std::hypot(px - (ax + t * vx), py - (ay + t * vy)) - lerpf(ra, rb, t);
}

void rasterizeCreatureInStrip(const ScenePrim& p, int stripY0, int stripH, int W, uint8_t* lumBuf, uint8_t* modeBuf,
                              bool nocturne) {
  const int yStart = std::max<int>(stripY0, p.box.y0);
  const int yEnd = std::min<int>(stripY0 + stripH - 1, p.box.y1);
  if (yStart > yEnd) return;
  const int xStart = std::max<int>(0, p.box.x0);
  const int xEnd = std::min<int>(W - 1, p.box.x1);
  if (xStart > xEnd) return;

  const auto kind = static_cast<CreatureKind>(p.subKind);
  const float S = std::max(12.0f, p.r0);
  const float invS = 1.0f / S;
  const float ca = std::cos(p.angle);
  const float sa = std::sin(p.angle);
  const float facing = (p.curvature >= 0.0f) ? 1.0f : -1.0f;
  const float pxOutline = 1.15f * invS;
  const float pxHalo = 1.40f * invS;

  for (int y = yStart; y <= yEnd; ++y) {
    const int rowOff = (y - stripY0) * W;
    const float dy = static_cast<float>(y) - p.y0;
    for (int x = xStart; x <= xEnd; ++x) {
      const float dx = static_cast<float>(x) - p.x0;
      const float rotX = dx * ca + dy * sa;
      const float rotY = -dx * sa + dy * ca;
      const float u = (rotX * facing) * invS;
      const float v = rotY * invS;

      if (kind == CreatureKind::Hummingbird) {
        if (u < -1.25f || u > 1.42f || v < -1.18f || v > 0.95f) continue;

        // 1. Far background beating wing (tilted slightly forward-upward behind shoulder)
        const float dFarWing = sdCapsuleTapered(u, v, 0.08f, -0.18f, -0.34f, -0.92f, 0.14f, 0.06f);
        // 2. Near swept-back primary wing (sweeping from shoulder (0.02, -0.10) to wingtip (-0.98, -0.88))
        float dNearWing = 10.0f;
        float wingT = 0.0f;
        float wingPerp = 0.0f;
        {
          const float wx0 = 0.02f, wy0 = -0.10f, wx1 = -0.98f, wy1 = -0.88f;
          const float wdx = wx1 - wx0, wdy = wy1 - wy0;
          const float wLen = std::hypot(wdx, wdy);
          const float ux = wdx / wLen, uy = wdy / wLen;
          // Trailing edge bulges downward-back (+perp)!
          const float along = (u - wx0) * ux + (v - wy0) * uy;
          wingT = clampf(along / wLen, 0.0f, 1.0f);
          wingPerp = -(u - wx0) * uy + (v - wy0) * ux;  // <0 above leading edge, >0 toward trailing feathers
          const float chord = 0.34f * std::sin(std::pow(wingT, 0.52f) * kPi) *
                              (1.0f + 0.08f * std::cos(wingT * kPi * 11.0f));  // 6 scalloped primary feathers!
          if (along >= -0.04f && along <= wLen + 0.04f) {
            const float dLead = -wingPerp - 0.035f * (1.0f - 0.5f * wingT);
            const float dTrail = wingPerp - chord;
            const float dTips = std::max(-along, along - wLen);
            dNearWing = std::max({dLead, dTrail, dTips});
          }
        }

        // 3. Fanned tail feathers from rump (-0.28, 0.25) to (-0.86, 0.68)
        float dTail = 10.0f;
        for (int f = 0; f < 3; ++f) {
          const float ta = 2.34f + f * 0.19f;
          const float tl = (f == 1) ? 0.58f : 0.50f;
          dTail = std::min(dTail, sdCapsuleTapered(u, v, -0.26f, 0.22f, -0.26f + std::cos(ta) * tl,
                                                   0.22f + std::sin(ta) * tl, 0.065f, 0.045f));
        }

        // 4. Streamlined Head + Arched Neck + Plump Breast + Rump + Needle Bill
        const float dHead = std::hypot(u - 0.30f, v + 0.22f) - 0.175f;
        const float dChest = std::hypot(u - 0.12f, v + 0.04f) - 0.215f;
        const float dBelly = std::hypot(u + 0.08f, v - 0.11f) - 0.195f;
        const float dRump = std::hypot(u + 0.26f, v - 0.23f) - 0.125f;
        const float dTorso = smin2D(smin2D(dHead, dChest, 0.11f), smin2D(dBelly, dRump, 0.11f), 0.11f);
        // Slightly decurved needle bill reaching forward to (+1.26, -0.18)!
        const float dBeak = sdCapsuleTapered(u, v + 0.025f * std::sin(clampf((u - 0.44f) / 0.82f, 0.0f, 1.0f) * kPi),
                                             0.43f, -0.22f, 1.26f, -0.18f, 0.042f, 0.018f);

        const float dAll = std::min({dNearWing, dTorso, dBeak, dTail, dFarWing});
        if (dAll > pxHalo) continue;

        if (!nocturne) {
          if (dAll > 0.0f) {
            if (modeBuf[rowOff + x] != PM_FORCE_BLACK) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;
            }
            continue;
          }

          // Foreground priority: 1) Near Wing, 2) Beak, 3) Torso/Head, 4) Tail, 5) Far Wing!
          if (dNearWing <= 0.0f) {
            // Feather shaft lines radiating from shoulder to scalloped primaries!
            const float featherAngle = std::atan2(wingPerp + 0.02f, wingT * 1.1f + 0.05f);
            const bool shaftLine =
                (wingT > 0.18f && wingPerp > 0.02f) && (std::fabs(std::sin(featherAngle * 14.0f)) < 0.14f);
            if (dNearWing >= -pxOutline || shaftLine) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
            } else {
              const float wingLum = lerpf(92.0f, 232.0f, clampf(1.0f - wingPerp * 2.4f, 0.0f, 1.0f));
              lumBuf[rowOff + x] = static_cast<uint8_t>(wingLum);
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          } else if (dBeak <= 0.0f) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
          } else if (dTorso <= 0.0f) {
            const float dEye = std::hypot(u - 0.32f, v + 0.24f);
            if (dEye <= 0.055f) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;  // Crisp pupil!
            } else if (dEye <= 0.095f) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;  // Sparkling white eye-ring!
            } else if (dTorso >= -pxOutline) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
            } else if (u > 0.14f && v > -0.18f && v < 0.04f) {
              // Iridescent dark stippled hummingbird gorget throat!
              lumBuf[rowOff + x] = 68;
              modeBuf[rowOff + x] = PM_ATKINSON;
            } else {
              // Bright sunlit breast + shaded dorsal crown/back!
              const float bellyBright = clampf((u * 0.6f + v * 1.4f + 0.35f), 0.0f, 1.0f);
              lumBuf[rowOff + x] = static_cast<uint8_t>(lerpf(108.0f, 245.0f, bellyBright));
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          } else if (dTail <= 0.0f) {
            if (dTail >= -pxOutline * 0.9f) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
            } else {
              lumBuf[rowOff + x] = 115;
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          } else if (dFarWing <= 0.0f) {
            if (dFarWing >= -pxOutline * 0.9f) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
            } else {
              lumBuf[rowOff + x] = 118;
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          }
        } else {
          // Nocturne Hummingbird: velvet-black separation moat, crisp silver-white silhouette & needle bill,
          // carved black primary feather shafts, and living dark pupil inside a sparkling white eye-ring!
          if (dAll > 0.0f) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
            continue;
          }

          if (dNearWing <= 0.0f) {
            const float featherAngle = std::atan2(wingPerp + 0.02f, wingT * 1.1f + 0.05f);
            const bool shaftLine =
                (wingT > 0.18f && wingPerp > 0.02f) && (std::fabs(std::sin(featherAngle * 14.0f)) < 0.14f);
            if (dNearWing >= -pxOutline) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;
            } else if (shaftLine) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
            } else {
              const float wingLum = lerpf(86.0f, 236.0f, clampf(1.0f - wingPerp * 2.4f, 0.0f, 1.0f));
              lumBuf[rowOff + x] = static_cast<uint8_t>(wingLum);
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          } else if (dBeak <= 0.0f) {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;
          } else if (dTorso <= 0.0f) {
            const float dEye = std::hypot(u - 0.32f, v + 0.24f);
            if (dEye <= 0.055f) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;  // Living dark pupil!
            } else if (dEye <= 0.098f) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;  // Sparkling white eye-ring!
            } else if (dEye <= 0.128f) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;  // Dark eye-socket crease!
            } else if (dTorso >= -pxOutline) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;
            } else if (u > 0.14f && v > -0.18f && v < 0.04f) {
              lumBuf[rowOff + x] = 58;
              modeBuf[rowOff + x] = PM_ATKINSON;
            } else {
              const float bellyBright = clampf((u * 0.6f + v * 1.4f + 0.35f), 0.0f, 1.0f);
              lumBuf[rowOff + x] = static_cast<uint8_t>(lerpf(88.0f, 238.0f, bellyBright));
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          } else if (dTail <= 0.0f) {
            if (dTail >= -pxOutline * 0.9f) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;
            } else {
              lumBuf[rowOff + x] = 102;
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          } else if (dFarWing <= 0.0f) {
            if (dFarWing >= -pxOutline * 0.9f) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;
            } else {
              lumBuf[rowOff + x] = 108;
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          }
        }
      } else {
        // CreatureKind::TreeFrog — alert raised sitting posture resting directly atop a vine (v = 0)!
        if (u < -0.82f || u > 0.86f || v < -0.92f || v > 0.25f) continue;

        // 1. Alert 30-degree raised amphibian torso + rounded snout + prominent raised frog eye dome!
        const float bu = (u + 0.02f) * 0.90f - (v + 0.34f) * 0.43f;
        const float bv = (u + 0.02f) * 0.43f + (v + 0.34f) * 0.90f;
        const float dBack = (std::hypot(bu / 0.46f, bv / 0.24f) - 1.0f) * 0.24f;
        const float dSnout = sdCapsuleTapered(u, v, 0.22f, -0.42f, 0.56f, -0.34f, 0.21f, 0.115f);
        const float dEyeDome = std::hypot(u - 0.30f, v + 0.59f) - 0.145f;
        const float dVocalSac = std::hypot(u - 0.28f, v + 0.26f) - 0.17f;
        float dBody = smin2D(smin2D(dBack, dSnout, 0.10f), smin2D(dEyeDome, dVocalSac, 0.09f), 0.08f);
        dBody = std::max(dBody, v + 0.01f);

        // 2. Bold Z-folded foreground hind leg (thigh + calf) + sturdy front arm + sticky tree-frog toe discs!
        const float lu = (u + 0.20f) * 0.95f - (v + 0.17f) * 0.30f;
        const float lv = (u + 0.20f) * 0.30f + (v + 0.17f) * 0.95f;
        const float dHindLeg = (std::hypot(lu / 0.29f, lv / 0.145f) - 1.0f) * 0.145f;
        float dToes = sdCapsuleTapered(u, v, 0.19f, -0.26f, 0.34f, 0.02f, 0.082f, 0.060f);  // Sturdy forearm!
        for (int t = 0; t < 3; ++t) {
          dToes = std::min(dToes, std::hypot(u - (-0.24f + t * 0.095f), v - 0.03f) - 0.056f);  // Hind toe pads
          dToes = std::min(dToes, std::hypot(u - (0.29f + t * 0.085f), v - 0.03f) - 0.052f);   // Front toe pads
        }

        const float dAll = std::min({dHindLeg, dToes, dBody});
        if (dAll > pxHalo) continue;

        if (!nocturne) {
          if (dAll > 0.0f) {
            if (modeBuf[rowOff + x] != PM_FORCE_BLACK) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;
            }
            continue;
          }

          if (dToes <= 0.0f) {
            if (dToes >= -pxOutline * 0.95f) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
            } else {
              lumBuf[rowOff + x] = 172;
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          } else if (dHindLeg <= 0.0f) {
            if (dHindLeg >= -pxOutline) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
            } else {
              lumBuf[rowOff + x] = static_cast<uint8_t>(lerpf(122.0f, 224.0f, clampf(-lv * 4.6f + 0.45f, 0.0f, 1.0f)));
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          } else if (dHindLeg <= pxOutline * 1.25f && v < -0.10f) {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_FORCE_WHITE;  // Crisp white separation highlight over tucked thigh!
          } else {
            const float dEye = std::hypot(u - 0.31f, v + 0.58f);
            const bool mouthSmile = (u > 0.28f && u < 0.62f && std::fabs(v + 0.30f - (u - 0.45f) * 0.16f) < 0.025f);
            const bool dorsalSpot =
                (std::hypot(u + 0.04f, v + 0.44f) < 0.072f) || (std::hypot(u + 0.22f, v + 0.35f) < 0.060f);
            if (dEye <= 0.062f && std::fabs(v + 0.58f) < 0.036f) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;  // Horizontal frog pupil!
            } else if (dEye <= 0.108f) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;  // Bright iris!
            } else if (dEye <= 0.142f || dBody >= -pxOutline || mouthSmile) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
            } else if (dorsalSpot) {
              lumBuf[rowOff + x] = 65;
              modeBuf[rowOff + x] = PM_ATKINSON;
            } else if (v > -0.32f && u > 0.08f) {
              lumBuf[rowOff + x] = 246;
              modeBuf[rowOff + x] = PM_ATKINSON;  // Pale sunlit vocal sac & chest!
            } else {
              const float shade = clampf((-v - 0.14f) * 1.45f, 0.0f, 1.0f);
              lumBuf[rowOff + x] = static_cast<uint8_t>(lerpf(228.0f, 118.0f, shade));
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          }
        } else {
          // Nocturne Tree Frog: black moat over vine/leaves, crisp white amphibian contours,
          // horizontal dark pupil in a bright white iris, dark smiling mouth crease, and moonlit vocal sac!
          if (dAll > 0.0f) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;
            continue;
          }

          if (dToes <= 0.0f) {
            if (dToes >= -pxOutline * 0.95f) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;
            } else {
              lumBuf[rowOff + x] = 156;
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          } else if (dHindLeg <= 0.0f) {
            if (dHindLeg >= -pxOutline) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;
            } else {
              lumBuf[rowOff + x] = static_cast<uint8_t>(lerpf(96.0f, 214.0f, clampf(-lv * 4.6f + 0.45f, 0.0f, 1.0f)));
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          } else if (dHindLeg <= pxOutline * 1.25f && v < -0.10f) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_FORCE_BLACK;  // Dark separation crease over tucked thigh!
          } else {
            const float dEye = std::hypot(u - 0.31f, v + 0.58f);
            const bool mouthSmile = (u > 0.28f && u < 0.62f && std::fabs(v + 0.30f - (u - 0.45f) * 0.16f) < 0.025f);
            const bool dorsalSpot =
                (std::hypot(u + 0.04f, v + 0.44f) < 0.072f) || (std::hypot(u + 0.22f, v + 0.35f) < 0.060f);
            if (dEye <= 0.062f && std::fabs(v + 0.58f) < 0.036f) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;  // Horizontal dark frog pupil!
            } else if (dEye <= 0.108f) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;  // Bright moonlit iris!
            } else if (dEye <= 0.142f || mouthSmile) {
              lumBuf[rowOff + x] = 0;
              modeBuf[rowOff + x] = PM_FORCE_BLACK;
            } else if (dBody >= -pxOutline) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;
            } else if (dorsalSpot) {
              lumBuf[rowOff + x] = 48;
              modeBuf[rowOff + x] = PM_ATKINSON;
            } else if (v > -0.32f && u > 0.08f) {
              lumBuf[rowOff + x] = 232;
              modeBuf[rowOff + x] = PM_ATKINSON;
            } else {
              const float shade = clampf((-v - 0.14f) * 1.45f, 0.0f, 1.0f);
              lumBuf[rowOff + x] = static_cast<uint8_t>(lerpf(212.0f, 92.0f, shade));
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          }
        }
      }
    }
  }
}

void rasterizeWashInStrip(const ScenePrim& p, const ClearanceField& field, int stripY0, int stripH, int W,
                          uint8_t* lumBuf, uint8_t* modeBuf, bool nocturne) {
  const int yStart = std::max<int>(stripY0, p.box.y0);
  const int yEnd = std::min<int>(stripY0 + stripH - 1, p.box.y1);
  if (yStart > yEnd) return;
  const int xStart = std::max<int>(0, p.box.x0);
  const int xEnd = std::min<int>(W - 1, p.box.x1);
  if (xStart > xEnd) return;

  const float boxH = std::max(1.0f, p.y1 - p.y0);
  const float boxW = std::max(1.0f, p.x1 - p.x0);

  for (int y = yStart; y <= yEnd; ++y) {
    const int rowOff = (y - stripY0) * W;
    const float fy = static_cast<float>(y);
    for (int x = xStart; x <= xEnd; ++x) {
      if (modeBuf[rowOff + x] != PM_BG) continue;
      const float fx = static_cast<float>(x);
      const float c = field.clearanceAt(fx, fy);
      if (c < 16.0f) continue;
      const float fade = smoothstepf(16.0f, 52.0f, c);

      if (p.subKind == 0) {
        const float u = (fx - p.x0) / boxW;
        const float hFade = smoothstepf(0.03f, 0.22f, u) * smoothstepf(0.97f, 0.78f, u);
        if (hFade <= 0.002f) continue;
        const float ridgeH = p.y0 + boxH * (0.28f + 0.26f * fbm2D(u * 4.2f, 0.5f, p.seed));
        if (fy >= ridgeH) {
          const float depthBelow = (fy - ridgeH) / std::max(24.0f, boxH * 0.42f);
          if (depthBelow < 1.0f) {
            const float wisp = 0.68f + 0.32f * fbm2D(fx * 0.045f, fy * 0.07f, p.seed ^ 0x55AAu);
            const float washDark = std::pow(1.0f - depthBelow, 2.2f) * fade * hFade * wisp;
            if (!nocturne) {
              const float lum = lerpf(255.0f, static_cast<float>(p.lumA), washDark);
              if (lum < 245.0f) {
                lumBuf[rowOff + x] = static_cast<uint8_t>(lum);
                modeBuf[rowOff + x] = PM_ATKINSON;
              }
            } else {
              // Nocturne Shan-Shui mist: soft-edged ethereal moon-mist in wide open sky (zero box or pocket edges!)
              const float openFade = smoothstepf(30.0f, 76.0f, c);
              const float topSoft = smoothstepf(0.0f, 0.24f, depthBelow);
              const float mist = topSoft * std::pow(1.0f - depthBelow, 1.8f) * openFade * hFade * wisp;
              const float density = mist * 0.15f;
              if (density > 0.004f && hash2i(x, y, p.seed ^ 0x9E3779B9u) < density) {
                lumBuf[rowOff + x] = 255;
                modeBuf[rowOff + x] = PM_FORCE_WHITE;
              }
            }
          }
        }
      } else {
        // Smooth subtle Milky Way stardust sweep with elliptical end-taper (zero rectangular clipping!)
        const float nx = (fx - 0.5f * (p.x0 + p.x1)) / (boxW * 0.5f);
        const float ny = (fy - 0.5f * (p.y0 + p.y1)) / (boxH * 0.5f);
        const float endFade = smoothstepf(0.98f, 0.22f, std::hypot(nx, ny));
        if (endFade <= 0.002f) continue;
        const float bandDist = std::fabs(nx * 0.52f + ny * 0.85f + 0.14f * std::sin(nx * 2.4f));
        if (bandDist < 0.36f) {
          const float gauss = std::cos((bandDist / 0.36f) * kPi * 0.5f);
          const float cloud = 0.58f + 0.42f * fbm2D(fx * 0.035f, fy * 0.035f, p.seed ^ 0x3C6EF372u);
          const float strength = gauss * gauss * fade * endFade * cloud * 0.78f;
          if (!nocturne) {
            const float lum = lerpf(255.0f, static_cast<float>(p.lumA), strength);
            if (lum < 244.0f) {
              lumBuf[rowOff + x] = static_cast<uint8_t>(lum);
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          } else {
            // Nocturne Milky Way: scattered pinpoint stardust cloud fading seamlessly into night sky!
            const float density = strength * 0.17f;
            if (density > 0.003f && hash2i(x, y, p.seed ^ 0x85EBCA6Bu) < density) {
              lumBuf[rowOff + x] = 255;
              modeBuf[rowOff + x] = PM_FORCE_WHITE;
            }
          }
        }
      }
    }
  }
}

// Pixel runs drawn by the compositor, in row-major order. Replaying them
// redraws a page's ornaments without rebuilding the scene.
struct PixelRun {
  uint16_t y;
  uint16_t x;
  uint16_t len;
};

class RunRecorder {
 public:
  // About 8 KB; a scene with more runs is not cached.
  static constexpr size_t kMaxRuns = 1360;

  bool begin() {
    if (!runs_) runs_ = makeUniqueNoThrow<PixelRun[]>(kMaxRuns);
    count_ = 0;
    overflow_ = !runs_;
    return !overflow_;
  }
  void add(const int x, const int y) {
    if (overflow_) return;
    if (count_ > 0) {
      PixelRun& last = runs_[count_ - 1];
      if (last.y == y && last.x + last.len == x) {
        ++last.len;
        return;
      }
    }
    if (count_ == kMaxRuns) {
      overflow_ = true;
      return;
    }
    runs_[count_++] = {static_cast<uint16_t>(y), static_cast<uint16_t>(x), 1};
  }
  bool ok() const { return runs_ && !overflow_; }
  void replay(const GfxRenderer& renderer) const {
    for (size_t i = 0; i < count_; ++i) {
      const PixelRun& r = runs_[i];
      for (uint16_t k = 0; k < r.len; ++k) renderer.drawPixel(r.x + k, r.y, true);
    }
  }
  void release() {
    runs_.reset();
    count_ = 0;
    overflow_ = false;
  }

 private:
  std::unique_ptr<PixelRun[]> runs_;
  size_t count_ = 0;
  bool overflow_ = false;
};

void compositeAndDitherScene(const GfxRenderer& renderer, const ClearanceField& field, SceneGraph& sg, bool nocturne,
                             bool whiteBitVal, RunRecorder* recorder = nullptr) {
  if (sg.prims.empty()) return;
  // Only ink pixels are recordable; the reader always passes whiteBitVal = true.
  if (!whiteBitVal) recorder = nullptr;
  const auto plot = [&](const int x, const int y, const bool v) {
    renderer.drawPixel(x, y, v);
    if (recorder && v) recorder->add(x, y);
  };

  std::stable_sort(sg.prims.begin(), sg.prims.end(),
                   [](const ScenePrim& a, const ScenePrim& b) { return a.zLayer < b.zLayer; });

  const int W = field.screenW;
  const int H = field.usableBottom;
  const size_t stripPixels = static_cast<size_t>(W) * kStripRows;

  auto lumBuf = makeUniqueNoThrow<uint8_t[]>(stripPixels);
  auto modeBuf = makeUniqueNoThrow<uint8_t[]>(stripPixels);
  const size_t errWidth = static_cast<size_t>(W) + 4;
  auto errStorage = makeUniqueNoThrow<int16_t[]>(errWidth * 3);
  if (!lumBuf || !modeBuf || !errStorage) return;

  std::memset(errStorage.get(), 0, errWidth * 3 * sizeof(int16_t));
  int16_t* err0 = errStorage.get();
  int16_t* err1 = err0 + errWidth;
  int16_t* err2 = err1 + errWidth;

  for (int stripY0 = 0; stripY0 < H; stripY0 += kStripRows) {
    const int stripH = std::min(kStripRows, H - stripY0);
    const size_t curPixels = static_cast<size_t>(W) * stripH;
    std::memset(lumBuf.get(), nocturne ? 0 : 255, curPixels);
    std::memset(modeBuf.get(), PM_BG, curPixels);

    bool anyHit = false;
    for (const auto& prim : sg.prims) {
      if (prim.box.y1 < stripY0 || prim.box.y0 >= stripY0 + stripH) continue;
      anyHit = true;
      switch (prim.type) {
        case PrimType::Stroke:
          rasterizeStrokeInStrip(prim, stripY0, stripH, W, lumBuf.get(), modeBuf.get(), nocturne);
          break;
        case PrimType::Petal:
          rasterizePetalInStrip(prim, stripY0, stripH, W, lumBuf.get(), modeBuf.get(), nocturne);
          break;
        case PrimType::Leaf:
          rasterizeLeafInStrip(prim, stripY0, stripH, W, lumBuf.get(), modeBuf.get(), nocturne);
          break;
        case PrimType::Disc:
          rasterizeDiscInStrip(prim, stripY0, stripH, W, lumBuf.get(), modeBuf.get(), nocturne);
          break;
        case PrimType::Wash:
          rasterizeWashInStrip(prim, field, stripY0, stripH, W, lumBuf.get(), modeBuf.get(), nocturne);
          break;
        case PrimType::Creature:
          rasterizeCreatureInStrip(prim, stripY0, stripH, W, lumBuf.get(), modeBuf.get(), nocturne);
          break;
      }
    }

    if (!anyHit) {
      std::memset(errStorage.get(), 0, errWidth * 3 * sizeof(int16_t));
      continue;
    }

    // 1-Bit Atkinson Error Diffusion with crisp Solid-Ink & Knockout Moat isolation!
    for (int sy = 0; sy < stripH; ++sy) {
      const int y = stripY0 + sy;
      const int rowOff = sy * W;
      for (int x = 0; x < W; ++x) {
        const uint8_t m = modeBuf[rowOff + x];
        if (!nocturne) {
          if (m == PM_BG || m == PM_FORCE_WHITE) continue;

          // Hard mathematical safety gate: never emit a single pixel inside the text/card halo!
          if (field.clearanceAt(static_cast<float>(x), static_cast<float>(y)) <= 0.0f) continue;

          if (m == PM_FORCE_BLACK) {
            plot(x, y, true);
            continue;
          }

          const int rawLum = lumBuf[rowOff + x];
          if (rawLum >= 246) continue;  // Keep highlights sparkling clean white without stray dots!

          int adjusted = rawLum + err0[x + 2];
          if (adjusted < 0) adjusted = 0;
          if (adjusted > 255) adjusted = 255;

          const bool inkBlack = (adjusted < 128);
          const int quantVal = inkBlack ? 0 : 255;
          const int err = (adjusted - quantVal) >> 3;  // 1/8 Atkinson kernel (6/8 total diffusion)

          err0[x + 3] += static_cast<int16_t>(err);
          err0[x + 4] += static_cast<int16_t>(err);
          err1[x + 1] += static_cast<int16_t>(err);
          err1[x + 2] += static_cast<int16_t>(err);
          err1[x + 3] += static_cast<int16_t>(err);
          err2[x + 2] += static_cast<int16_t>(err);

          if (inkBlack) {
            plot(x, y, true);
          }
        } else {
          if (m == PM_BG || m == PM_FORCE_BLACK) continue;

          // Hard mathematical safety gate: never emit a single pixel inside the text/card halo!
          if (field.clearanceAt(static_cast<float>(x), static_cast<float>(y)) <= 0.0f) continue;

          if (m == PM_FORCE_WHITE) {
            plot(x, y, whiteBitVal);
            continue;
          }

          const int rawLum = lumBuf[rowOff + x];
          if (rawLum <= 12) continue;  // Keep deep night shadows velvet black without stray snow dots!
          if (rawLum >= 246) {
            plot(x, y, whiteBitVal);
            continue;
          }

          int adjusted = rawLum + err0[x + 2];
          if (adjusted < 0) adjusted = 0;
          if (adjusted > 255) adjusted = 255;

          const bool emitWhite = (adjusted >= 128);
          const int quantVal = emitWhite ? 255 : 0;
          const int err = (adjusted - quantVal) >> 3;  // 1/8 Atkinson kernel (6/8 total diffusion)

          err0[x + 3] += static_cast<int16_t>(err);
          err0[x + 4] += static_cast<int16_t>(err);
          err1[x + 1] += static_cast<int16_t>(err);
          err1[x + 2] += static_cast<int16_t>(err);
          err1[x + 3] += static_cast<int16_t>(err);
          err2[x + 2] += static_cast<int16_t>(err);

          if (emitWhite) {
            plot(x, y, whiteBitVal);
          }
        }
      }

      int16_t* tmp = err0;
      err0 = err1;
      err1 = err2;
      err2 = tmp;
      std::memset(err2, 0, errWidth * sizeof(int16_t));
    }
  }
}

}  // namespace

namespace {

// Last page's ornaments, keyed by the frame they were drawn on. Redraws of
// the same page (menu or lookup closed) replay instead of recomputing.
struct PageCache {
  RunRecorder runs;
  uint32_t key = 0;
  bool valid = false;
};
PageCache pageCache;

uint32_t fnv1a(uint32_t h, const void* data, const size_t len) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}

uint32_t pageCacheKey(const GfxRenderer& renderer, const RenderContext& ctx) {
  // Field by field: struct padding is not initialized.
  const int32_t fields[] = {static_cast<int32_t>(ctx.bookSeed),
                            ctx.spineIndex,
                            ctx.pageIndex,
                            ctx.fontId,
                            ctx.marginTop,
                            ctx.marginRight,
                            ctx.marginBottom,
                            ctx.marginLeft,
                            static_cast<int32_t>(ctx.theme),
                            ctx.darkMode ? 1 : 0,
                            renderer.getScreenWidth(),
                            renderer.getScreenHeight()};
  uint32_t h = fnv1a(2166136261u, fields, sizeof(fields));
  // The frame holds the text, highlights and status bar the scene avoids.
  return fnv1a(h, renderer.getFrameBuffer(), renderer.getBufferSize());
}

}  // namespace

void releaseOrnamentCache() {
  pageCache.runs.release();
  pageCache.valid = false;
}

void renderDeadSpaceOrnaments(const GfxRenderer& renderer, const Page& page, const RenderContext& ctx) {
  if (ctx.theme == Theme::Off) return;
  if (renderer.isFontCacheScanning() || !renderer.hasFrameBuffer()) return;

  const uint32_t key = pageCacheKey(renderer, ctx);
  if (pageCache.valid && pageCache.key == key) {
    pageCache.runs.replay(renderer);
    return;
  }
  pageCache.valid = false;

  ClearanceField field;
  int contentTop = 0;
  int contentBottom = 0;
  if (!buildClearanceField(renderer, page, ctx, field, contentTop, contentBottom)) return;

  std::vector<int16_t> cellComp;
  const auto regions = extractDeadRegions(field, contentTop, contentBottom, cellComp);
  if (regions.empty()) {
    // Cache "nothing to draw" too, so a redraw skips the field scan.
    pageCache.runs.begin();
    pageCache.key = key;
    pageCache.valid = pageCache.runs.ok();
    return;
  }

  const uint32_t pageSeed = ctx.bookSeed ^ (static_cast<uint32_t>(ctx.spineIndex + 1) * 0x9E3779B1u) ^
                            (static_cast<uint32_t>(ctx.pageIndex + 1) * 0x85EBCA77u);
  Rng rng(pageSeed);

  Theme resolvedTheme = ctx.theme;
  if (resolvedTheme == Theme::AutoCycle) {
    const uint32_t pick = ((ctx.bookSeed >> 3) + ctx.spineIndex + ctx.pageIndex) % 5u;
    resolvedTheme = static_cast<Theme>(1u + pick);
  }

  SceneGraph sg;
  sg.reserve(256);

  for (const auto& reg : regions) {
    switch (resolvedTheme) {
      case Theme::BotanicalFloral:
        buildBotanicalFloral(sg, field, reg, cellComp, rng);
        break;
      case Theme::PlumAndBamboo:
        buildPlumAndBamboo(sg, field, reg, rng);
        break;
      case Theme::CelestialAstrolabe:
        buildCelestialAstrolabe(sg, field, reg, cellComp, rng);
        break;
      case Theme::MorrisFiligree:
        buildMorrisFiligree(sg, field, reg, rng);
        break;
      case Theme::WavesAndPetals:
        buildWavesAndPetals(sg, field, reg, cellComp, rng);
        break;
      default:
        break;
    }
  }

  // In Reader Night Mode (ctx.darkMode == true), HalDisplay inverts at output (fb bit 0 -> visible white),
  // so emitting drawPixel(x, y, true) draws visible white Nocturne strokes on the inverted black page!
  const bool recording = pageCache.runs.begin();
  compositeAndDitherScene(renderer, field, sg, ctx.darkMode, /*whiteBitVal=*/true,
                          recording ? &pageCache.runs : nullptr);
  pageCache.key = key;
  pageCache.valid = pageCache.runs.ok();
}

void renderCanvasOrnaments(const GfxRenderer& renderer, Theme theme, uint32_t seed, const KeepoutRect* keepouts,
                           size_t keepoutCount, int marginPx, float haloPx, bool darkCanvas) {
  if (theme == Theme::Off) return;
  if (renderer.isFontCacheScanning() || !renderer.hasFrameBuffer()) return;
  const uint8_t* fb = renderer.getFrameBuffer();
  if (!fb) return;

  const int W = renderer.getScreenWidth();
  const int H = renderer.getScreenHeight();
  if (W <= 64 || H <= 64) return;

  const int gw = (W + kCellPx - 1) / kCellPx;
  const int gh = (H + kCellPx - 1) / kCellPx;
  const size_t totalCells = static_cast<size_t>(gw) * gh;

  auto dist16 = makeUniqueNoThrow<uint16_t[]>(totalCells);
  ClearanceField field;
  field.inkDistPx = makeUniqueNoThrow<uint8_t[]>(totalCells);
  if (!dist16 || !field.inkDistPx) return;

  field.screenW = W;
  field.screenH = H;
  field.gw = gw;
  field.gh = gh;
  field.usableBottom = std::max(64, H - std::max(marginPx, 4));
  field.haloPx = haloPx;
  field.edgePadPx = static_cast<float>(std::max(marginPx, 5));

  constexpr uint16_t kInf = 30000;
  for (size_t i = 0; i < totalCells; ++i) dist16[i] = kInf;

  int minInkY = H;
  int maxInkY = 0;
  for (int y = 0; y < H; ++y) {
    const int gy = y / kCellPx;
    for (int x = 0; x < W; ++x) {
      // On a light canvas (cleared to 0xFF), foreground ink is black (readLogicalInkBit == true).
      // On a dark canvas (cleared to 0x00), foreground ink is white (!readLogicalInkBit).
      const bool occupied = darkCanvas ? !readLogicalInkBit(renderer, fb, x, y) : readLogicalInkBit(renderer, fb, x, y);
      if (occupied) {
        dist16[gy * gw + (x / kCellPx)] = 0;
        minInkY = std::min(minInkY, y);
        maxInkY = std::max(maxInkY, y);
      }
    }
  }

  for (size_t i = 0; i < keepoutCount && keepouts; ++i) {
    const int gx0 = std::max(0, static_cast<int>(keepouts[i].x0) / kCellPx);
    const int gy0 = std::max(0, static_cast<int>(keepouts[i].y0) / kCellPx);
    const int gx1 = std::min(gw - 1, std::max(0, static_cast<int>(keepouts[i].x1)) / kCellPx);
    const int gy1 = std::min(gh - 1, std::max(0, static_cast<int>(keepouts[i].y1)) / kCellPx);
    for (int gy = gy0; gy <= gy1; ++gy) {
      for (int gx = gx0; gx <= gx1; ++gx) {
        dist16[gy * gw + gx] = 0;
      }
    }
    minInkY = std::min<int>(minInkY, keepouts[i].y0);
    maxInkY = std::max<int>(maxInkY, keepouts[i].y1);
  }

  // Two-pass Rosenfeld-Pfaltz Chamfer (12, 17) distance transform.
  constexpr uint16_t kOrth = 12;
  constexpr uint16_t kDiag = 17;
  for (int y = 0; y < gh; ++y) {
    for (int x = 0; x < gw; ++x) {
      uint16_t d = dist16[y * gw + x];
      if (d == 0) continue;
      if (x > 0) d = std::min<uint16_t>(d, dist16[y * gw + (x - 1)] + kOrth);
      if (y > 0) {
        d = std::min<uint16_t>(d, dist16[(y - 1) * gw + x] + kOrth);
        if (x > 0) d = std::min<uint16_t>(d, dist16[(y - 1) * gw + (x - 1)] + kDiag);
        if (x + 1 < gw) d = std::min<uint16_t>(d, dist16[(y - 1) * gw + (x + 1)] + kDiag);
      }
      dist16[y * gw + x] = d;
    }
  }
  for (int y = gh - 1; y >= 0; --y) {
    for (int x = gw - 1; x >= 0; --x) {
      uint16_t d = dist16[y * gw + x];
      if (x + 1 < gw) d = std::min<uint16_t>(d, dist16[y * gw + (x + 1)] + kOrth);
      if (y + 1 < gh) {
        d = std::min<uint16_t>(d, dist16[(y + 1) * gw + x] + kOrth);
        if (x + 1 < gw) d = std::min<uint16_t>(d, dist16[(y + 1) * gw + (x + 1)] + kDiag);
        if (x > 0) d = std::min<uint16_t>(d, dist16[(y + 1) * gw + (x - 1)] + kDiag);
      }
      dist16[y * gw + x] = d;
      const uint32_t px = (d + 1) / 3;
      field.inkDistPx[y * gw + x] = static_cast<uint8_t>(std::min<uint32_t>(255u, px));
    }
  }

  std::vector<int16_t> cellComp;
  const int cTop = (minInkY <= maxInkY) ? minInkY : marginPx;
  const int cBot = (minInkY <= maxInkY) ? maxInkY : (H - marginPx);
  const auto regions = extractDeadRegions(field, cTop, cBot, cellComp);
  if (regions.empty()) return;

  Rng rng(seed ^ 0xA53C9E21u);
  Theme resolvedTheme = theme;
  if (resolvedTheme == Theme::AutoCycle) {
    resolvedTheme = static_cast<Theme>(1u + (seed % 5u));
  }

  SceneGraph sg;
  sg.reserve(384);
  for (const auto& reg : regions) {
    switch (resolvedTheme) {
      case Theme::BotanicalFloral:
        buildBotanicalFloral(sg, field, reg, cellComp, rng);
        break;
      case Theme::PlumAndBamboo:
        buildPlumAndBamboo(sg, field, reg, rng);
        break;
      case Theme::CelestialAstrolabe:
        buildCelestialAstrolabe(sg, field, reg, cellComp, rng);
        break;
      case Theme::MorrisFiligree:
        buildMorrisFiligree(sg, field, reg, rng);
        break;
      case Theme::WavesAndPetals:
        buildWavesAndPetals(sg, field, reg, cellComp, rng);
        break;
      default:
        break;
    }
  }

  // On a direct dark canvas (cleared to 0x00 without output inversion), drawPixel(x, y, false) emits visible white!
  compositeAndDitherScene(renderer, field, sg, darkCanvas, /*whiteBitVal=*/false);
}

}  // namespace page_ornament
