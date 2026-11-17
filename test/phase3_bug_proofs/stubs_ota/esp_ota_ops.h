#pragma once
#include <cstddef>
#include <cstdint>

using esp_err_t = int;
using esp_ota_handle_t = uint32_t;
inline constexpr esp_err_t ESP_OK = 0;
inline constexpr size_t OTA_SIZE_UNKNOWN = 0xFFFFFFFFu;

struct esp_partition_t {
  int dummy = 0;
};

inline const char* esp_err_to_name(esp_err_t) { return "ESP_OK"; }
inline const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) { return nullptr; }
inline esp_err_t esp_ota_begin(const esp_partition_t*, size_t, esp_ota_handle_t*) { return -1; }
inline esp_err_t esp_ota_write(esp_ota_handle_t, const void*, size_t) { return -1; }
inline esp_err_t esp_ota_abort(esp_ota_handle_t) { return ESP_OK; }
inline esp_err_t esp_ota_end(esp_ota_handle_t) { return -1; }
inline esp_err_t esp_ota_set_boot_partition(const esp_partition_t*) { return -1; }
