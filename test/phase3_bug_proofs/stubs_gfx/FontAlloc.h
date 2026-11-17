#pragma once

#include <cstdlib>

inline void* fiFontMalloc(size_t size) { return std::malloc(size); }
inline void fiFontFree(void* ptr) { std::free(ptr); }
