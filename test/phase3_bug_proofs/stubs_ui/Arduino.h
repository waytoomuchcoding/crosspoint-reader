#pragma once
#include <WString.h>
#include <cstdint>

inline unsigned long g_uiTestMillis = 1000;
inline unsigned long millis() { return g_uiTestMillis; }
