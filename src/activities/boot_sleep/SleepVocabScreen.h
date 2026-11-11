#pragma once

class GfxRenderer;

namespace sleep_vocab {

// Renders an artsy sleep-screen composition of recently looked-up vocabulary cards
// (weighted toward recent lookups in the user's frequency "sweet spot", hiding
// overly easy words, showing the primary reading + 1-line summaries of secondary
// pronunciations) and fills the surrounding negative space with 1-bit procedural
// ornaments (floral vines + fauna, ink plum & bamboo, constellations, waves, etc.).
// Returns false if no eligible vocabulary entries exist so SleepActivity can fall back.
bool renderVocabSleepScreen(GfxRenderer& renderer, bool darkMode = false);

}  // namespace sleep_vocab
