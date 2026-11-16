// Included via -include in platformio.ini so NimBLE-Arduino builds with
// Peripheral + Broadcaster + 255-byte ATT MTU for CrossPoint Companion Share,
// without modifying the rebuilt ESP-IDF core's custom_sdkconfig.
#pragma once
#ifdef ESP_PLATFORM
#include <sdkconfig.h>

#undef CONFIG_BT_NIMBLE_ROLE_PERIPHERAL
#define CONFIG_BT_NIMBLE_ROLE_PERIPHERAL 1

#undef CONFIG_BT_NIMBLE_ROLE_BROADCASTER
#define CONFIG_BT_NIMBLE_ROLE_BROADCASTER 1

#undef CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU
#define CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU 255
#endif
