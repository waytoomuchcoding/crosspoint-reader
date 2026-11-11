#pragma once

#include <TextEncoding.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class BookMetadataCache;

namespace txtenc {

TextEncoding readEncodingOverride(const std::string& cachePath);
bool writeEncodingOverride(const std::string& cachePath, TextEncoding enc);

TextEncoding detectFileEncoding(const std::string& filepath, const std::string& cachePath = "",
                                bool* hasCjkOut = nullptr);

// Reads up to 768 raw bytes around `byteOffset` from `filepath` once and auto-detects the file's
// encoding so interactive preview screens can re-decode in RAM with zero SD I/O on Left/Right.
bool loadRawPreviewSample(const std::string& filepath, size_t byteOffset, std::vector<uint8_t>& rawOut,
                          TextEncoding& detectedOut, size_t& sampleOffsetOut);

void decodePreviewSample(const uint8_t* rawData, size_t rawLen, size_t sampleOffset, TextEncoding encoding,
                         TextEncoding detectedEncoding, std::string& utf8Out);

bool readEncodingPreview(const std::string& filepath, size_t byteOffset, TextEncoding encoding,
                         std::string& utf8PreviewOut, TextEncoding* detectedOut = nullptr);

size_t scanChapterHeadings(const std::string& filepath, TextEncoding activeEncoding, BookMetadataCache& cache,
                           bool& hasCjk);

}  // namespace txtenc
