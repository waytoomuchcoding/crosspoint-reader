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
    if (x < kScreenEdgePadPx || x >= static_cast<float>(screenW) - kScreenEdgePadPx) return -10.0f;
    if (y < kScreenEdgePadPx || y >= static_cast<float>(usableBottom) - 4.0f) return -10.0f;
    const float edgeDist = std::min({x - kScreenEdgePadPx, static_cast<float>(screenW - 1) - kScreenEdgePadPx - x,
                                     y - kScreenEdgePadPx, static_cast<float>(usableBottom) - 4.0f - y});
    const float inkClear = sampleInkDist(x, y) - kInkHaloPx;
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

      // Extract up to 5 greedy inscribed-circle poles inside this component.
      reg.poles.push_back({bestX, bestY, bestClear});
      for (int p = 1; p < 5; ++p) {
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

  std::sort(regions.begin(), regions.end(), [](const DeadRegion& a, const DeadRegion& b) { return a.areaPx > b.areaPx; });
  if (regions.size() > 4) regions.resize(4);
  return regions;
}

// ============================================================================
// Phase 2: Vector Scene Graph & Strip-Streamed 1-Bit Atkinson/Knockout Painter
// ============================================================================

enum PixelMode : uint8_t {
  PM_PAPER = 0,
  PM_ATKINSON = 1,
  PM_SOLID_INK = 2,
  PM_SOLID_WHITE = 3,
};

enum class PetalShape : uint8_t {
  WildRoseNotched = 0,
  PeonyRuffled = 1,
  CosmosFluted = 2,
  PlumRound = 3,
  SakuraCleft = 4,
};

enum class LeafStyle : uint8_t {
  WoodcutHalfShade = 0,
  SolidInkBamboo = 1,
  InkWashBamboo = 2,
};

enum class DiscKind : uint8_t {
  StamenCrown = 0,
  PlumCalyxPins = 1,
  Moon3D = 2,
  MorrisRosette = 3,
  StippledBud = 4,
  SeigaihaScale = 5,
  FacetedCompassStar = 6,
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
    const float reach = len + wid * 0.6f + 4.0f;
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
    const float reach = len + std::fabs(bend) * len + wid + 4.0f;
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

  void addDisc(uint8_t z, float cx, float cy, float radius, DiscKind kind, float phaseAngle = 0.0f,
               uint8_t lumA = 80, uint8_t lumB = 248, uint32_t seed = 0) {
    const float reach = radius * 1.45f + 5.0f;
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

void emitBotanicalFlower(SceneGraph& sg, float cx, float cy, float radius, int species, Rng& rng) {
  const float baseRot = rng.range(0.0f, kTwoPi);
  if (species == 0) {
    // Species 0: 5-Petal Tudor / Wild Botanical Rose (outer 5 notched petals + inner 5 cupped petals + stamen crown)
    const int nOuter = 5;
    for (int i = 0; i < nOuter; ++i) {
      const float a = baseRot + (kTwoPi * i) / nOuter + rng.range(-0.05f, 0.05f);
      sg.addPetal(4, cx, cy, a, radius * rng.range(0.95f, 1.04f), radius * 0.68f, PetalShape::WildRoseNotched, 54, 240,
                  rng.nextU32());
    }
    for (int i = 0; i < nOuter; ++i) {
      const float a = baseRot + (kTwoPi * (i + 0.5f)) / nOuter + rng.range(-0.06f, 0.06f);
      sg.addPetal(5, cx, cy, a, radius * rng.range(0.62f, 0.70f), radius * 0.48f, PetalShape::WildRoseNotched, 42, 232,
                  rng.nextU32());
    }
    sg.addDisc(7, cx, cy, radius * 0.27f, DiscKind::StamenCrown, baseRot, 55, 250, rng.nextU32());
  } else if (species == 1) {
    // Species 1: Layered Ruffled Peony / Camellia (3 lush concentric petal rings)
    const int nBack = 7;
    for (int i = 0; i < nBack; ++i) {
      const float a = baseRot + (kTwoPi * i) / nBack + rng.range(-0.06f, 0.06f);
      sg.addPetal(4, cx, cy, a, radius * rng.range(0.96f, 1.05f), radius * 0.56f, PetalShape::PeonyRuffled, 52, 242,
                  rng.nextU32());
    }
    const int nMid = 6;
    for (int i = 0; i < nMid; ++i) {
      const float a = baseRot + 0.35f + (kTwoPi * i) / nMid + rng.range(-0.06f, 0.06f);
      sg.addPetal(5, cx, cy, a, radius * rng.range(0.70f, 0.78f), radius * 0.46f, PetalShape::PeonyRuffled, 46, 236,
                  rng.nextU32());
    }
    const int nFront = 5;
    for (int i = 0; i < nFront; ++i) {
      const float a = baseRot + 0.72f + (kTwoPi * i) / nFront;
      sg.addPetal(6, cx, cy, a, radius * 0.48f, radius * 0.34f, PetalShape::PeonyRuffled, 38, 228, rng.nextU32());
    }
    sg.addDisc(7, cx, cy, radius * 0.21f, DiscKind::StamenCrown, baseRot, 48, 248, rng.nextU32());
  } else {
    // Species 2: 8-Petal Fluted Botanical Cosmos / Aster with Fibonacci seedhead disk
    const int nRay = 8;
    for (int i = 0; i < nRay; ++i) {
      const float a = baseRot + (kTwoPi * i) / nRay + rng.range(-0.04f, 0.04f);
      const uint8_t z = (i % 2 == 0) ? 4 : 5;
      sg.addPetal(z, cx, cy, a, radius * rng.range(0.94f, 1.04f), radius * 0.36f, PetalShape::CosmosFluted, 50, 242,
                  rng.nextU32());
    }
    sg.addDisc(7, cx, cy, radius * 0.29f, DiscKind::StamenCrown, baseRot + 1.0f, 45, 242, rng.nextU32());
  }
}

void emitRoseBud(SceneGraph& sg, float bx, float by, float angle, float size, Rng& rng) {
  const float ux = std::cos(angle);
  const float uy = std::sin(angle);
  sg.addPetal(5, bx, by, angle, size, size * 0.56f, PetalShape::PlumRound, 85, 246, rng.nextU32());
  // Two embracing outer sepals in crisp ink.
  sg.addLeaf(6, bx - ux * 1.5f, by - uy * 1.5f, angle + 0.42f, size * 0.72f, size * 0.18f, -0.18f,
             LeafStyle::SolidInkBamboo, false, rng.nextU32());
  sg.addLeaf(6, bx - ux * 1.5f, by - uy * 1.5f, angle - 0.42f, size * 0.72f, size * 0.18f, 0.18f,
             LeafStyle::SolidInkBamboo, false, rng.nextU32());
}

void emitPlumBlossom(SceneGraph& sg, float cx, float cy, float radius, bool sideCupView, Rng& rng) {
  const float rot = rng.range(0.0f, kTwoPi);
  if (!sideCupView) {
    for (int i = 0; i < 5; ++i) {
      const float a = rot + (kTwoPi * i) / 5.0f + rng.range(-0.04f, 0.04f);
      sg.addPetal((i % 2 == 0) ? 5 : 6, cx, cy, a, radius, radius * 0.72f, PetalShape::PlumRound, 145, 254,
                  rng.nextU32());
    }
    sg.addDisc(7, cx, cy, radius * 0.36f, DiscKind::PlumCalyxPins, rot, 40, 255, rng.nextU32());
  } else {
    // 3/4 cupped view with 4 foreshortened petals and crisp 3-dot calyx base.
    for (int i = -1; i <= 2; ++i) {
      const float a = rot + i * 0.52f;
      sg.addPetal(5 + (i == 0 ? 1 : 0), cx, cy, a, radius * 0.90f, radius * 0.55f, PetalShape::PlumRound, 138, 253,
                  rng.nextU32());
    }
    sg.addDisc(7, cx - std::cos(rot + 0.25f) * radius * 0.18f, cy - std::sin(rot + 0.25f) * radius * 0.18f,
               radius * 0.22f, DiscKind::StippledBud, rot, 35, 110, rng.nextU32());
  }
}

void emitSakuraFlowerOrPetal(SceneGraph& sg, float cx, float cy, float radius, bool fullBloom, Rng& rng) {
  const float rot = rng.range(0.0f, kTwoPi);
  if (fullBloom) {
    for (int i = 0; i < 5; ++i) {
      const float a = rot + (kTwoPi * i) / 5.0f;
      sg.addPetal((i % 2 == 0) ? 5 : 6, cx, cy, a, radius, radius * 0.64f, PetalShape::SakuraCleft, 132, 254,
                  rng.nextU32());
    }
    sg.addDisc(7, cx, cy, radius * 0.30f, DiscKind::PlumCalyxPins, rot, 55, 255, rng.nextU32());
  } else {
    // Single tumbling notched sakura petal in the wind.
    sg.addPetal(6, cx, cy, rot, radius, radius * rng.range(0.54f, 0.72f), PetalShape::SakuraCleft, 140, 254,
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
    const float cx = (reg.x0 + reg.x1) * 0.5f;
    const float flowerR = clampf(mainPole.clearance * 0.78f, 8.5f, 15.5f);
    const float halfSpan = std::min(155.0f, (reg.x1 - reg.x0) * 0.40f);

    for (int dir = -1; dir <= 1; dir += 2) {
      std::vector<std::pair<float, float>> stem;
      const int nPts = 14;
      for (int i = 0; i <= nPts; ++i) {
        const float t = static_cast<float>(i) / nPts;
        const float px = cx + dir * t * halfSpan;
        const float py = cy + std::sin(t * kPi * 1.6f) * std::min(5.5f, mainPole.clearance * 0.30f) * dir;
        if (field.clearanceAt(px, py) < 2.5f) break;
        stem.push_back({px, py});
        if (i >= 3 && i <= 10 && i % 3 == 0) {
          const float leafAng = (dir > 0 ? 0.0f : kPi) + ((i % 2 == 0) ? -0.68f : 0.68f);
          const float leafLen = clampf(flowerR * (1.25f - 0.45f * t), 8.0f, 16.0f);
          sg.addLeaf(3, px, py, leafAng, leafLen, leafLen * 0.42f, (i % 2 == 0) ? 0.14f : -0.14f,
                     LeafStyle::WoodcutHalfShade, true, rng.nextU32());
        }
      }
      emitCurvedPolyline(sg, 2, stem, 1.35f, 0.65f, true, false, rng.nextU32());
      if (stem.size() >= 4) {
        const auto& tip = stem.back();
        emitRoseBud(sg, tip.first, tip.second, dir > 0 ? 0.0f : kPi, flowerR * 0.68f, rng);
      }
    }
    emitBotanicalFlower(sg, cx, cy, flowerR, 0, rng);
    return;
  }

  // Major Expanse: Space-colonization botanical vine skeleton + hero blooms at clearance poles!
  struct VineNode {
    float x, y;
    int parent;
    int depth;
    float dirX, dirY;
  };

  std::vector<VineNode> nodes;
  nodes.reserve(120);
  nodes.push_back({mainPole.x, mainPole.y, -1, 0, 0.0f, -1.0f});

  // Sample Poisson-like attractor cloud inside this connected component (seeding secondary poles first!).
  struct Attractor {
    float x, y;
    bool active;
  };
  std::vector<Attractor> attractors;
  const int targetAttr = std::clamp(reg.areaPx / 850, 36, 165);
  attractors.reserve(targetAttr + reg.poles.size());
  for (size_t p = 1; p < reg.poles.size(); ++p) {
    attractors.push_back({reg.poles[p].x, reg.poles[p].y, true});
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
      if (dx * dx + dy * dy < 24.0f * 24.0f) {
        tooClose = true;
        break;
      }
    }
    if (!tooClose) attractors.push_back({ax, ay, true});
  }

  // Space colonization iterations with clearance-gradient steering.
  constexpr float kStepLen = 15.0f;
  constexpr float kKillDist = 20.0f;
  constexpr float kInfluenceDist = 115.0f;

  std::vector<float> pullX(nodes.size(), 0.0f);
  std::vector<float> pullY(nodes.size(), 0.0f);
  std::vector<int> pullCount(nodes.size(), 0);

  for (int iter = 0; iter < 48 && nodes.size() < 210; ++iter) {
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
    for (size_t ni = 0; ni < curCount && nodes.size() < 210; ++ni) {
      if (pullCount[ni] == 0) continue;
      float dx = pullX[ni] / static_cast<float>(pullCount[ni]);
      float dy = pullY[ni] / static_cast<float>(pullCount[ni]);
      // Blend parent momentum + clearance gradient so vines curve organically away from text!
      float gx = 0.0f, gy = 0.0f;
      field.gradientAt(nodes[ni].x, nodes[ni].y, gx, gy);
      const float cHere = field.clearanceAt(nodes[ni].x, nodes[ni].y);
      const float wallPush = (cHere < 24.0f) ? 0.52f : 0.12f;
      dx = dx * 0.72f + nodes[ni].dirX * 0.28f + gx * wallPush;
      dy = dy * 0.72f + nodes[ni].dirY * 0.28f + gy * wallPush;
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
        if (ex * ex + ey * ey < (kStepLen * 0.68f) * (kStepLen * 0.68f)) {
          crowd = true;
          break;
        }
      }
      if (!crowd) {
        nodes.push_back({nx, ny, static_cast<int>(ni), nodes[ni].depth + 1, dx, dy});
      }
    }
  }

  // Count children per node so we know tips vs internal branches.
  std::vector<int> childCount(nodes.size(), 0);
  int maxDepth = 1;
  for (size_t i = 1; i < nodes.size(); ++i) {
    childCount[nodes[i].parent]++;
    maxDepth = std::max(maxDepth, nodes[i].depth);
  }

  // Emit smooth quadratic-subdivided stem segments, alternating serrated botanical leaves, buds & tendrils!
  for (size_t i = 1; i < nodes.size(); ++i) {
    const auto& cur = nodes[i];
    const auto& par = nodes[cur.parent];
    const float t0 = static_cast<float>(par.depth) / static_cast<float>(maxDepth + 1);
    const float t1 = static_cast<float>(cur.depth) / static_cast<float>(maxDepth + 1);
    const float r0 = lerpf(1.95f, 0.62f, t0);
    const float r1 = lerpf(1.95f, 0.55f, t1);
    sg.addStroke(2, par.x, par.y, cur.x, cur.y, r0, r1, true, false, false, static_cast<uint32_t>(i));

    const float segAng = std::atan2(cur.y - par.y, cur.x - par.x);
    const float localClear = field.clearanceAt(cur.x, cur.y);

    // Place woodcut leaves at alternating phyllotaxis angles along stems!
    if (localClear >= 11.0f && (i % 2 == 1 || childCount[i] == 0)) {
      const float side = (i % 2 == 0) ? 1.0f : -1.0f;
      const float leafAng = segAng + side * rng.range(0.58f, 1.02f);
      const float leafLen = clampf(localClear * rng.range(0.68f, 0.92f), 11.0f, 27.0f);
      const float tipX = cur.x + std::cos(leafAng) * leafLen;
      const float tipY = cur.y + std::sin(leafAng) * leafLen;
      if (field.clearanceAt(tipX, tipY) >= 4.0f) {
        sg.addLeaf(3, cur.x, cur.y, leafAng, leafLen, leafLen * rng.range(0.38f, 0.48f), side * 0.16f,
                   LeafStyle::WoodcutHalfShade, true, rng.nextU32());
      }
    }

    // Terminal shoot accents: spiral tendrils, rosebuds, or small secondary wildflowers!
    if (childCount[i] == 0) {
      if (localClear >= 19.0f && rng.chance(0.42f)) {
        const float subR = clampf(localClear * 0.58f, 12.0f, 21.0f);
        emitBotanicalFlower(sg, cur.x, cur.y, subR, rng.rangeInt(0, 2), rng);
      } else if (localClear >= 12.0f && rng.chance(0.55f)) {
        emitRoseBud(sg, cur.x, cur.y, segAng, clampf(localClear * 0.65f, 9.0f, 15.0f), rng);
      } else if (localClear >= 9.0f) {
        emitSpiralTendril(sg, field, cur.x, cur.y, segAng, clampf(localClear * 0.85f, 9.0f, 18.0f),
                          (i % 2 == 0) ? 1.0f : -1.0f, 2);
      }
    }
  }

  // Hero blossoms anchored directly on the closest vine node to each clearance pole!
  const size_t bloomCount = std::min<size_t>(reg.poles.size(), 3);
  for (size_t p = 0; p < bloomCount; ++p) {
    const auto& rawPole = reg.poles[p];
    Pole pole = rawPole;
    if (p > 0 && !nodes.empty()) {
      size_t bestIdx = 0;
      float bestD = 1e9f;
      for (size_t ni = 0; ni < nodes.size(); ++ni) {
        const float d = std::hypot(nodes[ni].x - rawPole.x, nodes[ni].y - rawPole.y);
        if (d < bestD) {
          bestD = d;
          bestIdx = ni;
        }
      }
      if (bestD < 64.0f) {
        pole.x = nodes[bestIdx].x;
        pole.y = nodes[bestIdx].y;
        pole.clearance = field.clearanceAt(pole.x, pole.y);
      } else {
        continue;
      }
    }
    const float maxR = (p == 0) ? 44.0f : 29.0f;
    const float bloomR = clampf(pole.clearance * (p == 0 ? 0.54f : 0.46f), 15.0f, maxR);
    if (p > 0 && bloomR < 15.5f) continue;

    // Frame the hero bloom with 3 radial backing leaves before drawing petals!
    const int nGuardLeaves = (p == 0) ? 4 : 2;
    const float guardRot = rng.range(0.0f, kTwoPi);
    for (int k = 0; k < nGuardLeaves; ++k) {
      const float la = guardRot + (kTwoPi * k) / nGuardLeaves + rng.range(-0.22f, 0.22f);
      const float lx = pole.x + std::cos(la) * (bloomR * 0.58f);
      const float ly = pole.y + std::sin(la) * (bloomR * 0.58f);
      const float lLen = bloomR * rng.range(0.68f, 0.86f);
      if (field.clearanceAt(lx + std::cos(la) * lLen, ly + std::sin(la) * lLen) >= 5.0f) {
        sg.addLeaf(3, lx, ly, la, lLen, lLen * 0.44f, (k % 2 == 0) ? 0.14f : -0.14f, LeafStyle::WoodcutHalfShade,
                   true, rng.nextU32());
      }
    }

    const int species = (p == 0) ? rng.rangeInt(0, 2) : static_cast<int>((p + rng.nextU32()) % 3);
    emitBotanicalFlower(sg, pole.x, pole.y, bloomR, species, rng);
  }
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
    const float blossomR = clampf(mainPole.clearance * 0.72f, 8.0f, 13.5f);

    // Calligraphic angular plum twig across the scene break.
    std::vector<std::pair<float, float>> twig = {
        {cx - span, cy + 3.0f},   {cx - span * 0.45f, cy - 2.5f}, {cx - span * 0.1f, cy + 2.0f},
        {cx + span * 0.38f, cy - 3.0f}, {cx + span * 0.92f, cy + 1.5f}};
    emitCurvedPolyline(sg, 2, twig, 2.1f, 0.7f, true, true, rng.nextU32());
    emitPlumBlossom(sg, cx - span * 0.18f, cy + 1.0f, blossomR, false, rng);
    emitPlumBlossom(sg, cx + span * 0.34f, cy - 2.5f, blossomR * 0.84f, true, rng);
    sg.addDisc(6, cx - span * 0.56f, cy - 4.0f, blossomR * 0.34f, DiscKind::StippledBud, 0.0f, 95, 248, rng.nextU32());
    sg.addDisc(6, cx + span * 0.68f, cy - 2.0f, blossomR * 0.30f, DiscKind::StippledBud, 0.0f, 105, 248, rng.nextU32());
    return;
  }

  // Distant Shan-Shui mountain mist wash in wide tailpieces!
  if (reg.kind == RegionKind::TailpieceExpanse && (reg.y1 - reg.y0) >= 120) {
    sg.addWash(reg.x0, reg.y0 + (reg.y1 - reg.y0) / 3, reg.x1, reg.y1, 0, 178, rng.nextU32());
  }

  // Choose an anchor side to launch the gnarled calligraphic plum trunk toward mainPole!
  const bool fromLeft = rng.chance(0.55f);
  float startX = fromLeft ? static_cast<float>(reg.x0 + 18) : static_cast<float>(reg.x1 - 18);
  float startY = clampf(mainPole.y + (reg.y1 - mainPole.y) * 0.45f, static_cast<float>(reg.y0 + 22),
                        static_cast<float>(reg.y1 - 22));
  // Snap start point inward until clearance >= 12.
  for (int s = 0; s < 16 && field.clearanceAt(startX, startY) < 12.0f; ++s) {
    startX = lerpf(startX, mainPole.x, 0.18f);
    startY = lerpf(startY, mainPole.y, 0.18f);
  }

  // Recursive angular brush branch builder ("iron-wire" calligraphy kinks + feibai dry brush).
  auto growPlumBranch = [&](auto& self, float x, float y, float ang, float segLen, float radius, int depth) -> void {
    if (depth > 6 || segLen < 10.0f) return;
    float nx = x + std::cos(ang) * segLen;
    float ny = y + std::sin(ang) * segLen;
    float c = field.clearanceAt(nx, ny);
    if (c < 7.0f) {
      float gx = 0.0f, gy = 0.0f;
      field.gradientAt(x, y, gx, gy);
      ang = std::atan2(std::sin(ang) * 0.45f + gy * 0.55f, std::cos(ang) * 0.45f + gx * 0.55f);
      nx = x + std::cos(ang) * (segLen * 0.72f);
      ny = y + std::sin(ang) * (segLen * 0.72f);
      c = field.clearanceAt(nx, ny);
      if (c < 6.5f) return;
    }

    const float nextR = std::max(0.65f, radius * 0.74f);
    sg.addStroke(2, x, y, nx, ny, radius, nextR, true, radius >= 2.1f, false, rng.nextU32());

    // Blossoms and pearl buds at branch joints & slender twigs!
    if (depth >= 2 && c >= 14.0f && rng.chance(depth >= 3 ? 0.68f : 0.42f)) {
      const float bR = clampf(c * rng.range(0.36f, 0.52f), 10.5f, 22.0f);
      emitPlumBlossom(sg, nx, ny, bR, rng.chance(0.35f), rng);
    } else if (depth >= 2 && c >= 8.0f && rng.chance(0.55f)) {
      const float budAng = ang + (rng.chance(0.5f) ? 0.6f : -0.6f);
      const float bx = nx + std::cos(budAng) * 5.5f;
      const float by = ny + std::sin(budAng) * 5.5f;
      if (field.clearanceAt(bx, by) >= 6.0f) {
        sg.addDisc(6, bx, by, rng.range(3.6f, 5.4f), DiscKind::StippledBud, budAng, 92, 248, rng.nextU32());
      }
    }

    // Calligraphic angular bend + fork!
    const float kink = (depth % 2 == 0 ? 1.0f : -1.0f) * rng.range(0.24f, 0.48f);
    self(self, nx, ny, ang + kink, segLen * rng.range(0.78f, 0.88f), nextR, depth + 1);
    if (depth <= 4 && rng.chance(0.72f)) {
      const float forkAng = ang - kink * rng.range(1.4f, 2.1f);
      self(self, nx, ny, forkAng, segLen * rng.range(0.62f, 0.78f), nextR * 0.82f, depth + 2);
    }
  };

  const float mainAng = std::atan2(mainPole.y - startY, mainPole.x - startX);
  const float trunkDist = std::hypot(mainPole.x - startX, mainPole.y - startY);
  const float trunkLen = clampf(trunkDist * 0.42f, 24.0f, 56.0f);
  // First reach mainPole with a 2-segment gnarled trunk so the hero blossom always sits right on a branch!
  const float midX = lerpf(startX, mainPole.x, 0.52f) + (fromLeft ? 8.0f : -8.0f);
  const float midY = lerpf(startY, mainPole.y, 0.52f) + 10.0f;
  if (field.clearanceAt(midX, midY) >= 8.0f) {
    sg.addStroke(2, startX, startY, midX, midY, 3.6f, 2.7f, true, true, false, rng.nextU32());
    sg.addStroke(2, midX, midY, mainPole.x, mainPole.y, 2.7f, 2.0f, true, true, false, rng.nextU32());
    growPlumBranch(growPlumBranch, midX, midY, mainAng - 0.52f, trunkLen * 0.72f, 2.1f, 2);
  } else {
    sg.addStroke(2, startX, startY, mainPole.x, mainPole.y, 3.3f, 2.1f, true, true, false, rng.nextU32());
  }
  growPlumBranch(growPlumBranch, mainPole.x, mainPole.y, mainAng + 0.22f, trunkLen * 0.82f, 2.1f, 1);
  growPlumBranch(growPlumBranch, mainPole.x, mainPole.y, mainAng - 0.58f, trunkLen * 0.68f, 1.7f, 2);

  // Focal plum blossom directly on the gnarled main joint!
  const float heroR = clampf(mainPole.clearance * 0.44f, 14.0f, 25.0f);
  emitPlumBlossom(sg, mainPole.x, mainPole.y, heroR, false, rng);

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
      // Leave a crisp 2.2px white node gap between bamboo segments!
      const float gapRatio = 2.4f / segH;
      const float sx1 = lerpf(bx, nx, 1.0f - gapRatio);
      const float sy1 = lerpf(by, ny, 1.0f - gapRatio);
      sg.addStroke(2, bx, by, sx1, sy1, rCulm, rCulm * 0.92f, true, false, false, rng.nextU32());
      // Crisp horizontal bamboo node tick!
      const float px = -std::sin(curAng) * (rCulm + 2.2f);
      const float py = std::cos(curAng) * (rCulm + 2.2f);
      sg.addStroke(3, sx1 - px, sy1 - py, sx1 + px, sy1 + py, 0.85f, 0.85f, false);

      // Classic '个' (3-4 leaf) swept bamboo clusters: foreground solid black + background Atkinson ink-wash!
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
// Theme 3: Celestial Astrolabe (3D Dithered Moon, Degree Rings & Constellations)
// ============================================================================

void buildCelestialAstrolabe(SceneGraph& sg, const ClearanceField& field, const DeadRegion& reg,
                             const std::vector<int16_t>& cellComp, Rng& rng) {
  if (reg.poles.empty()) return;
  const Pole& mainPole = reg.poles[0];

  if (reg.kind == RegionKind::SceneBreakCorridor) {
    const float cy = mainPole.y;
    const float cx = (reg.x0 + reg.x1) * 0.5f;
    const float moonR = clampf(mainPole.clearance * 0.65f, 7.0f, 12.0f);
    const float spacing = moonR * 3.1f;
    const float phases[5] = {-2.15f, -1.15f, 0.0f, 1.15f, 2.15f};
    const float axisHalf = std::min(165.0f, (reg.x1 - reg.x0) * 0.42f);
    sg.addStroke(1, cx - axisHalf, cy, cx + axisHalf, cy, 0.55f, 0.55f, false, false, true);
    for (int i = 0; i < 5; ++i) {
      const float mx = cx + (i - 2) * spacing;
      if (field.clearanceAt(mx, cy) >= moonR + 2.0f) {
        const float rHere = (i == 2) ? moonR * 1.15f : moonR * 0.88f;
        sg.addDisc(6, mx, cy, rHere, DiscKind::Moon3D, phases[i], 30, 250, rng.nextU32());
      }
    }
    sg.addDisc(7, cx - axisHalf, cy, 5.5f, DiscKind::FacetedCompassStar, 0.0f);
    sg.addDisc(7, cx + axisHalf, cy, 5.5f, DiscKind::FacetedCompassStar, 0.0f);
    return;
  }

  // Soft Milky Way stardust nebula across the dead space!
  sg.addWash(reg.x0, reg.y0, reg.x1, reg.y1, 1, 188, rng.nextU32());

  const float astroR = clampf(mainPole.clearance * 0.62f, 20.0f, 54.0f);

  // Outer graduated astrolabe degree rings + ticks.
  auto emitClippedCircle = [&](float cx, float cy, float r, float strokeR, bool dotted, int segs = 72) {
    for (int i = 0; i < segs; ++i) {
      if (dotted && (i % 2 == 1)) continue;
      const float a0 = (kTwoPi * i) / segs;
      const float a1 = (kTwoPi * (i + 1)) / segs;
      const float x0 = cx + std::cos(a0) * r;
      const float y0 = cy + std::sin(a0) * r;
      const float x1 = cx + std::cos(a1) * r;
      const float y1 = cy + std::sin(a1) * r;
      if (field.clearanceAt(x0, y0) >= 5.0f && field.clearanceAt(x1, y1) >= 5.0f) {
        sg.addStroke(2, x0, y0, x1, y1, strokeR, strokeR, false);
      }
    }
  };

  emitClippedCircle(mainPole.x, mainPole.y, astroR, 1.05f, false, 84);
  emitClippedCircle(mainPole.x, mainPole.y, astroR * 0.86f, 0.65f, false, 72);
  // Wide orbital arcs extending into surrounding dead space (clipped smoothly by clearance!).
  emitClippedCircle(mainPole.x, mainPole.y, astroR * 1.38f, 0.60f, true, 96);
  emitClippedCircle(mainPole.x, mainPole.y, astroR * 1.78f, 0.55f, true, 120);

  // Off-center ecliptic ring inside astrolabe.
  emitClippedCircle(mainPole.x + astroR * 0.14f, mainPole.y - astroR * 0.10f, astroR * 0.64f, 0.60f, false, 60);

  // 36 degree graduation ticks around the astrolabe bezel.
  for (int i = 0; i < 36; ++i) {
    const float a = (kTwoPi * i) / 36.0f;
    const float innerR = (i % 3 == 0) ? astroR * 0.80f : astroR * 0.88f;
    sg.addStroke(3, mainPole.x + std::cos(a) * innerR, mainPole.y + std::sin(a) * innerR,
                 mainPole.x + std::cos(a) * astroR, mainPole.y + std::sin(a) * astroR, 0.6f, 0.6f, false);
  }

  // 4 cardinal compass-rose faceted pointers.
  for (int k = 0; k < 4; ++k) {
    const float a = k * kPi * 0.5f;
    const float sx = mainPole.x + std::cos(a) * (astroR * 1.04f);
    const float sy = mainPole.y + std::sin(a) * (astroR * 1.04f);
    if (field.clearanceAt(sx, sy) >= 8.0f) {
      sg.addDisc(7, sx, sy, clampf(astroR * 0.19f, 5.5f, 9.5f), DiscKind::FacetedCompassStar, a);
    }
  }

  // Focal 3D Lambertian cratered Moon at center of astrolabe!
  const float phaseChoices[4] = {-1.25f, -0.55f, 0.65f, 1.35f};
  const float moonPhase = phaseChoices[rng.rangeInt(0, 3)];
  sg.addDisc(6, mainPole.x, mainPole.y, astroR * 0.45f, DiscKind::Moon3D, moonPhase, 25, 252, rng.nextU32());

  // Constellation star graph filling the rest of the irregular dead space!
  struct StarPt {
    float x, y;
    float mag;
  };
  std::vector<StarPt> stars;
  const int targetStars = std::clamp(reg.areaPx / 2200, 10, 42);
  for (int attempt = 0; attempt < targetStars * 8 && static_cast<int>(stars.size()) < targetStars; ++attempt) {
    const float sx = rng.range(static_cast<float>(reg.x0 + 10), static_cast<float>(reg.x1 - 10));
    const float sy = rng.range(static_cast<float>(reg.y0 + 10), static_cast<float>(reg.y1 - 10));
    const int gx = std::clamp(static_cast<int>(sx) / kCellPx, 0, field.gw - 1);
    const int gy = std::clamp(static_cast<int>(sy) / kCellPx, 0, field.gh - 1);
    if (cellComp[gy * field.gw + gx] != reg.componentId) continue;
    if (field.clearanceAt(sx, sy) < 9.0f) continue;
    if (std::hypot(sx - mainPole.x, sy - mainPole.y) < astroR * 1.18f) continue;

    bool tooClose = false;
    for (const auto& st : stars) {
      if (std::hypot(sx - st.x, sy - st.y) < 26.0f) {
        tooClose = true;
        break;
      }
    }
    if (!tooClose) stars.push_back({sx, sy, rng.nextFloat()});
  }

  // Relative-neighborhood / Gabriel graph edges for crisp planar constellations!
  for (size_t i = 0; i < stars.size(); ++i) {
    for (size_t j = i + 1; j < stars.size(); ++j) {
      const float d = std::hypot(stars[j].x - stars[i].x, stars[j].y - stars[i].y);
      if (d < 26.0f || d > 82.0f) continue;
      const float mx = 0.5f * (stars[i].x + stars[j].x);
      const float my = 0.5f * (stars[i].y + stars[j].y);
      if (field.clearanceAt(mx, my) < 7.0f) continue;
      if (std::hypot(mx - mainPole.x, my - mainPole.y) < astroR * 1.10f) continue;

      bool gabriel = true;
      for (size_t k = 0; k < stars.size(); ++k) {
        if (k == i || k == j) continue;
        if (std::hypot(stars[k].x - mx, stars[k].y - my) < d * 0.52f) {
          gabriel = false;
          break;
        }
      }
      if (gabriel) {
        const float ux = (stars[j].x - stars[i].x) / d;
        const float uy = (stars[j].y - stars[i].y) / d;
        const float gap = 5.5f;
        sg.addStroke(2, stars[i].x + ux * gap, stars[i].y + uy * gap, stars[j].x - ux * gap, stars[j].y - uy * gap,
                     0.58f, 0.58f, false);
      }
    }
  }

  for (const auto& st : stars) {
    if (st.mag > 0.72f && field.clearanceAt(st.x, st.y) >= 12.0f) {
      sg.addDisc(7, st.x, st.y, rng.range(5.5f, 8.5f), DiscKind::FacetedCompassStar, rng.range(0.0f, 0.4f));
    } else {
      sg.addDisc(7, st.x, st.y, (st.mag > 0.35f) ? 2.1f : 1.3f, DiscKind::StippledBud, 0.0f, 0, 20);
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

  const bool symmetric = (reg.kind != RegionKind::MarginalBay) &&
                         (std::fabs(mainPole.x - field.screenW * 0.5f) < field.screenW * 0.18f);
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
  const float colStep = scaleR * 1.72f;
  const float rowStep = scaleR * 0.86f;
  const float waterTopBase =
      lerpf(static_cast<float>(reg.y0), static_cast<float>(reg.y1), (reg.kind == RegionKind::MarginalBay) ? 0.48f : 0.38f);
  const float swellPhase = rng.range(0.0f, kTwoPi);

  int rowIdx = 0;
  for (float wy = waterTopBase; wy <= static_cast<float>(reg.y1) + scaleR * 0.5f; wy += rowStep, ++rowIdx) {
    // Front (lower) rows get higher zLayer so lower wave crests cleanly knock out upper rows!
    const uint8_t zWave = static_cast<uint8_t>(std::min(5, 2 + (rowIdx % 4)));
    const float xOffset = (rowIdx % 2 == 1) ? colStep * 0.5f : 0.0f;
    for (float wx = static_cast<float>(reg.x0) + xOffset; wx <= static_cast<float>(reg.x1); wx += colStep) {
      const float swellY = std::sin(wx * 0.022f + swellPhase) * 14.0f;
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

void rasterizeStrokeInStrip(const ScenePrim& p, int stripY0, int stripH, int W, uint8_t* lumBuf, uint8_t* modeBuf) {
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
      const float dist = std::hypot(fx - projX, fy - projY);
      const float rHere = lerpf(p.r0, p.r1, t);

      if (dist <= rHere) {
        if (dryBrush && rHere >= 1.9f && dist < rHere - 1.05f) {
          // Calligraphic feibai (飞白) bark grain inside thick plum trunks!
          const float grain = valueNoise2D(fx * 0.45f, fy * 0.45f, p.seed);
          if (grain > 0.74f) {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_SOLID_WHITE;
            continue;
          }
        }
        lumBuf[rowOff + x] = 0;
        modeBuf[rowOff + x] = PM_SOLID_INK;
      } else if (whiteHalo && dist <= rHere + 1.45f) {
        if (modeBuf[rowOff + x] != PM_SOLID_INK) {
          lumBuf[rowOff + x] = 255;
          modeBuf[rowOff + x] = PM_SOLID_WHITE;
        }
      }
    }
  }
}

void rasterizePetalInStrip(const ScenePrim& p, int stripY0, int stripH, int W, uint8_t* lumBuf, uint8_t* modeBuf) {
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
      if (u < -0.06f || u > 1.12f) continue;

      // Parametric botanical lamina silhouette half-width(u).
      const float uc = clampf(u, 0.0f, 1.0f);
      float profile = 0.0f;
      float tipLimit = 1.0f;
      switch (shape) {
        case PetalShape::WildRoseNotched: {
          profile = std::sin(std::pow(uc, 0.72f) * kPi) * (0.62f + 0.55f * uc);
          const float vNormEst = std::fabs(perp) / std::max(1.0f, p.width * 0.5f);
          tipLimit = 1.0f - 0.11f * std::exp(-vNormEst * vNormEst * 9.0f);  // Gentle heart notch at tip
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
          // Nearly circular overlapping Song-dynasty plum petal.
          const float du = (u - 0.54f) / 0.54f;
          profile = (std::fabs(du) < 1.0f) ? std::sqrt(std::max(0.0f, 1.0f - du * du)) : 0.0f;
          tipLimit = 1.04f;
          break;
        }
        case PetalShape::SakuraCleft: {
          profile = std::sin(std::pow(uc, 0.74f) * kPi) * (0.65f + 0.48f * uc);
          const float vNormEst = std::fabs(perp) / std::max(1.0f, p.width * 0.5f);
          // Classic sharp V-shaped sakura petal notch (欠け) at tip!
          tipLimit = 1.02f - 0.22f * std::max(0.0f, 1.0f - vNormEst * 3.2f);
          break;
        }
      }

      const float halfW = p.width * 0.5f * profile;
      // Approximate signed Euclidean distance to petal margin (negative inside, positive outside).
      const float dSide = std::fabs(perp) - halfW;
      const float dTip = (u - tipLimit) * p.length;
      const float dBase = -u * p.length;
      const float sdf = std::max({dSide, dTip, dBase});

      if (sdf > 1.55f) continue;

      if (sdf > 0.0f) {
        // Crisp white paper knockout separation halo so overlapping petals never turn into black blobs!
        lumBuf[rowOff + x] = 255;
        modeBuf[rowOff + x] = PM_SOLID_WHITE;
      } else if (sdf >= -1.18f) {
        // Razor-sharp 1-bit black ink contour!
        lumBuf[rowOff + x] = 0;
        modeBuf[rowOff + x] = PM_SOLID_INK;
      } else if (sdf >= -2.35f) {
        // Crisp 1px sunlit white paper rim just inside the ink contour so dithered shading pops cleanly!
        lumBuf[rowOff + x] = 255;
        modeBuf[rowOff + x] = PM_SOLID_WHITE;
      } else {
        // Interior tonal cup shading + delicate botanical petal venation!
        const float vRel = perp / std::max(1.5f, halfW);
        float tCup = std::pow(clampf(u / std::max(0.2f, tipLimit), 0.0f, 1.0f), 0.92f);
        float lum = lerpf(static_cast<float>(p.lumA), static_cast<float>(p.lumB), tCup);
        lum += 10.0f * (vRel * vRel);

        // Delicate radiating botanical veins in the basal 72% of the petal!
        if (u > 0.08f && u < 0.74f) {
          const float veinWave = std::fabs(std::sin(vRel * (shape == PetalShape::CosmosFluted ? 4.8f : 6.2f)));
          if (veinWave < 0.15f) {
            lum -= (1.0f - u / 0.74f) * 62.0f;
          }
        }
        lumBuf[rowOff + x] = static_cast<uint8_t>(clampf(lum, 24.0f, 255.0f));
        modeBuf[rowOff + x] = PM_ATKINSON;
      }
    }
  }
}

void rasterizeLeafInStrip(const ScenePrim& p, int stripY0, int stripH, int W, uint8_t* lumBuf, uint8_t* modeBuf) {
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
      if (u < -0.04f || u > 1.06f) continue;

      const float uc = clampf(u, 0.0f, 1.0f);
      const float spineOffset = p.curvature * p.length * uc * uc;
      const float perp = (-rx * sa + ry * ca) - spineOffset;

      // Lanceolate / ovate leaf blade envelope with pointed acuminate tip.
      float env = std::sin(std::pow(uc, 0.68f) * kPi) * (1.0f - 0.25f * uc);
      if (serrated && uc > 0.16f && uc < 0.92f) {
        env *= (1.0f + 0.085f * std::sin(uc * 34.0f));
      }
      const float halfW = p.width * 0.5f * env;
      const float dSide = std::fabs(perp) - halfW;
      const float dTip = (u - 1.0f) * p.length;
      const float dBase = -u * p.length;
      const float sdf = std::max({dSide, dTip, dBase});

      if (sdf > 1.35f) continue;

      if (sdf > 0.0f) {
        if (modeBuf[rowOff + x] != PM_SOLID_INK) {
          lumBuf[rowOff + x] = 255;
          modeBuf[rowOff + x] = PM_SOLID_WHITE;
        }
        continue;
      }

      if (style == LeafStyle::SolidInkBamboo) {
        // Pure crisp 1-bit ink silhouette with slender midrib highlight on wide blades.
        if (p.width >= 6.5f && std::fabs(perp) < 0.55f && uc > 0.12f && uc < 0.72f) {
          lumBuf[rowOff + x] = 255;
          modeBuf[rowOff + x] = PM_SOLID_WHITE;
        } else {
          lumBuf[rowOff + x] = 0;
          modeBuf[rowOff + x] = PM_SOLID_INK;
        }
      } else if (style == LeafStyle::InkWashBamboo) {
        // Background 50% Atkinson ink-wash bamboo leaf for classical brush depth!
        lumBuf[rowOff + x] = static_cast<uint8_t>(lerpf(118.0f, 175.0f, uc));
        modeBuf[rowOff + x] = PM_ATKINSON;
      } else {
        // WoodcutHalfShade: crisp ink margin + central midrib + bilateral shaded/sunlit halves!
        if (sdf >= -1.12f || (std::fabs(perp) <= 0.68f && uc < 0.88f)) {
          lumBuf[rowOff + x] = 0;
          modeBuf[rowOff + x] = PM_SOLID_INK;
        } else if (perp > 0.0f) {
          // Shaded half of leaf: Atkinson midtone + diagonal lateral woodcut veins!
          const float diagVein = std::sin((uc * p.length - perp * 0.85f) * 1.15f);
          float lum = lerpf(118.0f, 205.0f, uc * 0.65f + (perp / std::max(1.0f, halfW)) * 0.35f);
          if (diagVein > 0.78f) lum -= 52.0f;
          lumBuf[rowOff + x] = static_cast<uint8_t>(clampf(lum, 30.0f, 245.0f));
          modeBuf[rowOff + x] = PM_ATKINSON;
        } else {
          // Sunlit half of leaf: crisp bright paper with sparse rim stipple!
          const float rim = clampf(-sdf / std::max(1.5f, halfW), 0.0f, 1.0f);
          lumBuf[rowOff + x] = (rim < 0.24f) ? 212 : 255;
          modeBuf[rowOff + x] = (rim < 0.24f) ? PM_ATKINSON : PM_SOLID_WHITE;
        }
      }
    }
  }
}

void rasterizeDiscInStrip(const ScenePrim& p, int stripY0, int stripH, int W, uint8_t* lumBuf, uint8_t* modeBuf) {
  const int yStart = std::max<int>(stripY0, p.box.y0);
  const int yEnd = std::min<int>(stripY0 + stripH - 1, p.box.y1);
  if (yStart > yEnd) return;
  const int xStart = std::max<int>(0, p.box.x0);
  const int xEnd = std::min<int>(W - 1, p.box.x1);
  if (xStart > xEnd) return;

  const auto kind = static_cast<DiscKind>(p.subKind);

  if (kind == DiscKind::StamenCrown || kind == DiscKind::PlumCalyxPins) {
    // Receptacle center + radiating filament hairlines + crisp black anther dots!
    const float coreR = p.r0 * (kind == DiscKind::PlumCalyxPins ? 0.36f : 0.56f);
    for (int y = yStart; y <= yEnd; ++y) {
      const int rowOff = (y - stripY0) * W;
      for (int x = xStart; x <= xEnd; ++x) {
        const float d = std::hypot(static_cast<float>(x) - p.x0, static_cast<float>(y) - p.y0);
        if (d <= coreR + 1.3f) {
          if (d > coreR) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_SOLID_INK;
          } else {
            lumBuf[rowOff + x] = (kind == DiscKind::PlumCalyxPins) ? 45 : 135;
            modeBuf[rowOff + x] = PM_ATKINSON;
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
      rasterizeStrokeInStrip(ray, stripY0, stripH, W, lumBuf, modeBuf);

      for (int y = std::max<int>(yStart, static_cast<int>(ay - 2)); y <= std::min<int>(yEnd, static_cast<int>(ay + 2));
           ++y) {
        for (int x = std::max<int>(xStart, static_cast<int>(ax - 2));
             x <= std::min<int>(xEnd, static_cast<int>(ax + 2)); ++x) {
          if (std::hypot(static_cast<float>(x) - ax, static_cast<float>(y) - ay) <= 1.35f) {
            lumBuf[(y - stripY0) * W + x] = 0;
            modeBuf[(y - stripY0) * W + x] = PM_SOLID_INK;
          }
        }
      }
    }
    return;
  }

  if (kind == DiscKind::FacetedCompassStar) {
    // Classic 4-point faceted compass star with alternating black & white bevels!
    for (int y = yStart; y <= yEnd; ++y) {
      const int rowOff = (y - stripY0) * W;
      const float dy = static_cast<float>(y) - p.y0;
      for (int x = xStart; x <= xEnd; ++x) {
        const float dx = static_cast<float>(x) - p.x0;
        const float r = std::hypot(dx, dy);
        if (r > p.r0 + 1.5f) continue;
        const float theta = std::atan2(dy, dx) - p.angle;
        const float starProfile = p.r0 * (0.24f + 0.76f * std::pow(std::fabs(std::cos(2.0f * theta)), 2.6f));
        if (r <= starProfile) {
          const int octant = static_cast<int>(std::floor(((theta + kTwoPi) / (kPi * 0.25f)))) & 1;
          if (r >= starProfile - 0.95f || octant == 0) {
            lumBuf[rowOff + x] = 0;
            modeBuf[rowOff + x] = PM_SOLID_INK;
          } else {
            lumBuf[rowOff + x] = 255;
            modeBuf[rowOff + x] = PM_SOLID_WHITE;
          }
        } else if (r <= starProfile + 1.35f && modeBuf[rowOff + x] != PM_SOLID_INK) {
          lumBuf[rowOff + x] = 255;
          modeBuf[rowOff + x] = PM_SOLID_WHITE;
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
        // Woodblock scalloped Seigaiha (青海波) upper semicircle wave scale!
        if (dy > p.r0 * 0.55f || dist > p.r0 + 1.4f) continue;
        if (dist > p.r0) {
          lumBuf[rowOff + x] = 255;
          modeBuf[rowOff + x] = PM_SOLID_WHITE;
          continue;
        }
        const float uRad = dist / p.r0;
        // 4 concentric crisp woodblock crest arcs + radial indigo-to-foam Atkinson tonal wash!
        const float ringWave = std::fabs(std::sin(uRad * kPi * 4.0f));
        if (dist >= p.r0 - 1.15f || (ringWave < 0.14f && uRad > 0.18f)) {
          lumBuf[rowOff + x] = 0;
          modeBuf[rowOff + x] = PM_SOLID_INK;
        } else if (uRad > 0.88f) {
          // Crisp white foam highlight just inside outer wave crest!
          lumBuf[rowOff + x] = 255;
          modeBuf[rowOff + x] = PM_SOLID_WHITE;
        } else {
          const float lum = lerpf(245.0f, static_cast<float>(p.lumA), std::pow(uRad, 1.35f));
          lumBuf[rowOff + x] = static_cast<uint8_t>(clampf(lum, 55.0f, 252.0f));
          modeBuf[rowOff + x] = PM_ATKINSON;
        }
        continue;
      }

      if (dist > p.r0 + 1.6f) continue;
      if (dist > p.r0) {
        lumBuf[rowOff + x] = 255;
        modeBuf[rowOff + x] = PM_SOLID_WHITE;
        continue;
      }
      if (dist >= p.r0 - 1.15f) {
        lumBuf[rowOff + x] = 0;
        modeBuf[rowOff + x] = PM_SOLID_INK;
        continue;
      }

      if (kind == DiscKind::Moon3D) {
        const float nx = dx / p.r0;
        const float ny = dy / p.r0;
        const float nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
        const float ndotl = (nx * lx + ny * ly + nz * lz) / lNorm;
        const float maria = fbm2D((nx + 1.5f) * 3.2f, (ny + 1.5f) * 3.2f, p.seed);
        float lum = 0.0f;
        if (ndotl <= -0.06f) {
          // Earthshine shadow side: deep velvety dark stipple with subtle limb glow.
          lum = 22.0f + 26.0f * (1.0f - nz);
        } else {
          const float lit = smoothstepf(-0.06f, 0.82f, ndotl);
          lum = lerpf(32.0f, 255.0f, lit) - (1.0f - maria) * 48.0f * lit;
        }
        lumBuf[rowOff + x] = static_cast<uint8_t>(clampf(lum, 12.0f, 255.0f));
        modeBuf[rowOff + x] = PM_ATKINSON;
      } else if (kind == DiscKind::MorrisRosette) {
        const float uRad = dist / p.r0;
        const float theta = std::atan2(dy, dx) - p.angle;
        const float petalStar = 0.52f + 0.24f * std::cos(8.0f * theta);
        if (std::fabs(uRad - 0.78f) < 0.06f || std::fabs(uRad - 0.24f) < 0.07f) {
          lumBuf[rowOff + x] = 0;
          modeBuf[rowOff + x] = PM_SOLID_INK;
        } else if (uRad < petalStar && uRad > 0.28f) {
          lumBuf[rowOff + x] = 255;
          modeBuf[rowOff + x] = PM_SOLID_WHITE;
        } else {
          lumBuf[rowOff + x] = 128;
          modeBuf[rowOff + x] = PM_ATKINSON;
        }
      } else {
        // StippledBud / Berry / Star dot
        if (p.lumB <= 30) {
          lumBuf[rowOff + x] = 0;
          modeBuf[rowOff + x] = PM_SOLID_INK;
        } else {
          const float spec = std::hypot(dx / p.r0 + 0.32f, dy / p.r0 + 0.32f);
          const float lum = lerpf(255.0f, static_cast<float>(p.lumA), clampf(spec * 0.78f, 0.0f, 1.0f));
          lumBuf[rowOff + x] = static_cast<uint8_t>(lum);
          modeBuf[rowOff + x] = PM_ATKINSON;
        }
      }
    }
  }
}

void rasterizeWashInStrip(const ScenePrim& p, const ClearanceField& field, int stripY0, int stripH, int W,
                          uint8_t* lumBuf, uint8_t* modeBuf) {
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
      if (modeBuf[rowOff + x] != PM_PAPER) continue;
      const float fx = static_cast<float>(x);
      const float c = field.clearanceAt(fx, fy);
      if (c < 14.0f) continue;
      const float fade = smoothstepf(14.0f, 34.0f, c);

      if (p.subKind == 0) {
        // Distant Shan-Shui mountain ridge silhouette fading into misty white paper below!
        const float u = (fx - p.x0) / boxW;
        const float ridgeH = p.y0 + boxH * (0.28f + 0.26f * fbm2D(u * 4.2f, 0.5f, p.seed));
        if (fy >= ridgeH) {
          const float depthBelow = (fy - ridgeH) / std::max(24.0f, boxH * 0.42f);
          if (depthBelow < 1.0f) {
            const float washDark = (1.0f - depthBelow) * (1.0f - depthBelow) * fade;
            const float lum = lerpf(255.0f, static_cast<float>(p.lumA), washDark);
            if (lum < 245.0f) {
              lumBuf[rowOff + x] = static_cast<uint8_t>(lum);
              modeBuf[rowOff + x] = PM_ATKINSON;
            }
          }
        }
      } else {
        // Soft diagonal Milky Way stardust nebula band!
        const float nx = (fx - 0.5f * (p.x0 + p.x1)) / (boxW * 0.5f);
        const float ny = (fy - 0.5f * (p.y0 + p.y1)) / (boxH * 0.5f);
        const float bandDist = std::fabs(nx * 0.58f + ny * 0.81f + 0.18f * std::sin(nx * 3.0f));
        if (bandDist < 0.42f) {
          const float dust = fbm2D(fx * 0.035f, fy * 0.035f, p.seed);
          const float strength = (1.0f - bandDist / 0.42f) * dust * fade;
          const float lum = lerpf(255.0f, static_cast<float>(p.lumA), strength);
          if (lum < 244.0f) {
            lumBuf[rowOff + x] = static_cast<uint8_t>(lum);
            modeBuf[rowOff + x] = PM_ATKINSON;
          }
        }
      }
    }
  }
}

void compositeAndDitherScene(const GfxRenderer& renderer, const ClearanceField& field, SceneGraph& sg) {
  if (sg.prims.empty()) return;

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
    std::memset(lumBuf.get(), 255, curPixels);
    std::memset(modeBuf.get(), PM_PAPER, curPixels);

    bool anyHit = false;
    for (const auto& prim : sg.prims) {
      if (prim.box.y1 < stripY0 || prim.box.y0 >= stripY0 + stripH) continue;
      anyHit = true;
      switch (prim.type) {
        case PrimType::Stroke:
          rasterizeStrokeInStrip(prim, stripY0, stripH, W, lumBuf.get(), modeBuf.get());
          break;
        case PrimType::Petal:
          rasterizePetalInStrip(prim, stripY0, stripH, W, lumBuf.get(), modeBuf.get());
          break;
        case PrimType::Leaf:
          rasterizeLeafInStrip(prim, stripY0, stripH, W, lumBuf.get(), modeBuf.get());
          break;
        case PrimType::Disc:
          rasterizeDiscInStrip(prim, stripY0, stripH, W, lumBuf.get(), modeBuf.get());
          break;
        case PrimType::Wash:
          rasterizeWashInStrip(prim, field, stripY0, stripH, W, lumBuf.get(), modeBuf.get());
          break;
      }
    }

    if (!anyHit) {
      std::memset(errStorage.get(), 0, errWidth * 3 * sizeof(int16_t));
      continue;
    }

    // 1-Bit Atkinson Error Diffusion with crisp Solid-Ink & Paper-Knockout isolation!
    for (int sy = 0; sy < stripH; ++sy) {
      const int y = stripY0 + sy;
      const int rowOff = sy * W;
      for (int x = 0; x < W; ++x) {
        const uint8_t m = modeBuf[rowOff + x];
        if (m == PM_PAPER || m == PM_SOLID_WHITE) continue;

        // Hard mathematical safety gate: never emit a single black pixel inside the text halo!
        if (field.clearanceAt(static_cast<float>(x), static_cast<float>(y)) <= 0.0f) continue;

        if (m == PM_SOLID_INK) {
          renderer.drawPixel(x, y, true);
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
          renderer.drawPixel(x, y, true);
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

void renderDeadSpaceOrnaments(const GfxRenderer& renderer, const Page& page, const RenderContext& ctx) {
  if (ctx.theme == Theme::Off) return;
  if (renderer.isFontCacheScanning() || !renderer.hasFrameBuffer()) return;

  ClearanceField field;
  int contentTop = 0;
  int contentBottom = 0;
  if (!buildClearanceField(renderer, page, ctx, field, contentTop, contentBottom)) return;

  std::vector<int16_t> cellComp;
  const auto regions = extractDeadRegions(field, contentTop, contentBottom, cellComp);
  if (regions.empty()) return;

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

  compositeAndDitherScene(renderer, field, sg);
}

}  // namespace page_ornament
