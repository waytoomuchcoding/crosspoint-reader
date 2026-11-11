#pragma once

class SdCardFontSystem {
 public:
  bool dirty = false;
  void markRegistryDirty() { dirty = true; }
};

inline SdCardFontSystem sdFontSystem;
