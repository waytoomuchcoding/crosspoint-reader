#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace zh {

struct JumpCandidate {
  uint16_t wordIndex;
  uint32_t rank;
  uint8_t inDict;
};

// Sorts visible page words from rarest to most common (dictionary words first,
// then highest frequency rank / NO_RANK, tie-broken by reading order).
inline void buildJumpOrder(std::vector<JumpCandidate>& candidates, std::vector<uint16_t>& outOrder) {
  std::sort(candidates.begin(), candidates.end(), [](const JumpCandidate& a, const JumpCandidate& b) {
    if (a.inDict != b.inDict) return a.inDict > b.inDict;
    if (a.rank != b.rank) return a.rank > b.rank;
    return a.wordIndex < b.wordIndex;
  });
  outOrder.clear();
  outOrder.reserve(candidates.size());
  for (const auto& c : candidates) outOrder.push_back(c.wordIndex);
}

// Fixed-size state for the 2-press, 4-column button-slice jump mode.
// Each batch holds up to 16 of the rarest visible words, ordered in reading
// order so buttons 0..3 correspond to top-to-bottom quarters of the page.
struct JumpBatch {
  static constexpr uint8_t MAX_TARGETS = 16;
  static constexpr int PICK_NOOP = -1;
  static constexpr int PICK_NARROWED = -2;

  struct TargetCode {
    uint8_t col1 = 0;    // 0..3: first button press
    uint8_t col2 = 0;    // 0..3: second button press (unused when slices == 1)
    uint8_t slices = 1;  // 1 or 2
  };

  uint16_t targets[MAX_TARGETS] = {};
  uint8_t count = 0;
  uint16_t offset = 0;
  int8_t firstPress = -1;  // -1 = awaiting 1st press, 0..3 = 1st press chosen

  void clear() {
    count = 0;
    offset = 0;
    firstPress = -1;
  }

  void load(const std::vector<uint16_t>& order, const uint16_t startOffset) {
    firstPress = -1;
    if (order.empty()) {
      count = 0;
      offset = 0;
      return;
    }
    offset = startOffset < order.size() ? startOffset : 0;
    const size_t avail = order.size() - offset;
    count = static_cast<uint8_t>(avail < MAX_TARGETS ? avail : MAX_TARGETS);
    for (uint8_t i = 0; i < count; ++i) targets[i] = order[offset + i];
    // Reading order so button 0..3 map top-to-bottom across the page.
    std::sort(targets, targets + count);
  }

  void nextBatch(const std::vector<uint16_t>& order) {
    if (order.empty()) {
      clear();
      return;
    }
    const size_t next = static_cast<size_t>(offset) + MAX_TARGETS;
    load(order, next < order.size() ? static_cast<uint16_t>(next) : 0);
  }

  bool prevBatch(const std::vector<uint16_t>& order) {
    if (offset < MAX_TARGETS) return false;
    load(order, static_cast<uint16_t>(offset - MAX_TARGETS));
    return true;
  }

  // Balanced 4-way split of `count`: for count == 16 each group has 4 items;
  // for smaller batches every active button gets 1..4 items.
  void groupRange(const uint8_t col, uint8_t& first, uint8_t& groupCount) const {
    const uint8_t n = count;
    const uint8_t g = n < 4 ? n : 4;
    if (g == 0 || col >= g) {
      first = 0;
      groupCount = 0;
      return;
    }
    uint8_t pos = 0;
    for (uint8_t b = 0; b < col; ++b) pos += static_cast<uint8_t>(n / g + (b < n % g ? 1 : 0));
    first = pos;
    groupCount = static_cast<uint8_t>(n / g + (col < n % g ? 1 : 0));
  }

  TargetCode codeFor(const uint8_t index) const {
    const uint8_t n = count;
    const uint8_t g = n < 4 ? n : 4;
    uint8_t pos = 0;
    for (uint8_t b = 0; b < g; ++b) {
      const uint8_t sz = static_cast<uint8_t>(n / g + (b < n % g ? 1 : 0));
      if (index < pos + sz) {
        return {b, static_cast<uint8_t>(index - pos), static_cast<uint8_t>(sz > 1 ? 2 : 1)};
      }
      pos += sz;
    }
    return {};
  }

  // Returns wordIndex (>= 0) when a target is chosen, PICK_NARROWED when
  // filtered to the 2nd press, or PICK_NOOP if `col` has no target.
  int pick(const uint8_t col) {
    if (col >= 4 || count == 0) return PICK_NOOP;
    if (firstPress < 0) {
      uint8_t first = 0, groupCount = 0;
      groupRange(col, first, groupCount);
      if (groupCount == 0) return PICK_NOOP;
      if (groupCount == 1) return targets[first];
      firstPress = static_cast<int8_t>(col);
      return PICK_NARROWED;
    }
    uint8_t first = 0, groupCount = 0;
    groupRange(static_cast<uint8_t>(firstPress), first, groupCount);
    if (col >= groupCount) return PICK_NOOP;
    return targets[first + col];
  }

  // Undoes the first press, or steps back to the previous batch of 16.
  // Returns false when already at the top of batch 0 (caller exits jump mode).
  bool undo(const std::vector<uint16_t>& order) {
    if (firstPress >= 0) {
      firstPress = -1;
      return true;
    }
    return prevBatch(order);
  }
};

}  // namespace zh
