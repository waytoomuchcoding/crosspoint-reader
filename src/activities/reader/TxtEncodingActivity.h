#pragma once

#include <TextEncoding.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class TxtEncodingActivity final : public Activity {
 public:
  TxtEncodingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                      std::string cachePath, size_t previewByteOffset = 0)
      : Activity("TxtEncoding", renderer, mappedInput),
        bookPath_(std::move(bookPath)),
        cachePath_(std::move(cachePath)),
        previewByteOffset_(previewByteOffset) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void refreshPreview();
  TextEncoding currentEncoding() const;

  ButtonNavigator navigator_;
  std::string bookPath_;
  std::string cachePath_;
  size_t previewByteOffset_ = 0;
  size_t sampleOffset_ = 0;
  std::vector<uint8_t> rawSample_;
  TextEncoding initialEncoding_ = TextEncoding::Auto;
  TextEncoding detectedEncoding_ = TextEncoding::Utf8;
  int position_ = 0;
  std::string previewText_;
};
