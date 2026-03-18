#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Slot type stored in NVS.
 */
typedef enum {
    ESP_STORAGE_NVS_EMPTY  = 0,
    ESP_STORAGE_NVS_INT    = 1,
    ESP_STORAGE_NVS_STRING = 2,
    ESP_STORAGE_NVS_NUMBER = 3,
} esp_storage_nvs_type_t;

/**
 * @brief Initialize storage subsystem.
 *
 * Requirements:
 * - nvs_flash_init() must be called before this function.
 * - A LittleFS partition labeled "storage" must exist in partition table.
 *
 * Default configuration:
 * - NVS namespace: "storage"
 * - LittleFS base path: "/littlefs"
 * - LittleFS partition label: "storage"
 */
esp_err_t esp_storage_init(bool log_enabled, bool at_enabled);

/**
 * @brief Deinitialize storage subsystem and unmount LittleFS.
 */
esp_err_t esp_storage_deinit(void);

/**
 * @brief Report whether storage subsystem is initialized.
 */
bool esp_storage_is_initialized(void);

/**
 * @brief Set integer value to NVS slot [0..255].
 */
esp_err_t esp_storage_nvs_set_int(uint16_t slot, int64_t value);

/**
 * @brief Get integer value from NVS slot [0..255].
 */
esp_err_t esp_storage_nvs_get_int(uint16_t slot, int64_t *out_value);

/**
 * @brief Set floating-point number value to NVS slot [0..255].
 */
esp_err_t esp_storage_nvs_set_number(uint16_t slot, double value);

/**
 * @brief Get floating-point number value from NVS slot [0..255].
 */
esp_err_t esp_storage_nvs_get_number(uint16_t slot, double *out_value);

/**
 * @brief Set string value to NVS slot [0..255].
 */
esp_err_t esp_storage_nvs_set_string(uint16_t slot, const char *value);

/**
 * @brief Get string value from NVS slot [0..255].
 *
 * @param[in]  slot      Slot index.
 * @param[out] out       Destination buffer.
 * @param[in]  out_len   Destination buffer length.
 * @param[out] req_len   Required length including trailing '\0' (optional).
 */
esp_err_t esp_storage_nvs_get_string(uint16_t slot, char *out, size_t out_len, size_t *req_len);

/**
 * @brief Get NVS slot type.
 */
esp_err_t esp_storage_nvs_get_type(uint16_t slot, esp_storage_nvs_type_t *out_type);

/**
 * @brief Check whether NVS slot contains active data.
 *
 * Active means the slot has a stored type marker, even if the stored integer is 0
 * or the stored string is empty.
 */
esp_err_t esp_storage_nvs_is_active(uint16_t slot, bool *out_active);

/**
 * @brief Erase NVS slot data.
 */
esp_err_t esp_storage_nvs_erase(uint16_t slot);

/**
 * @brief Write binary payload to LittleFS slot [0..255].
 *
 * The payload is written to /littlefs/slot_XXX.bin.
 */
esp_err_t esp_storage_lfs_write(uint16_t slot, const void *data, size_t len);

/**
 * @brief Read binary payload from LittleFS slot [0..255].
 *
 * @param[in]  slot      Slot index.
 * @param[out] out       Destination buffer.
 * @param[in]  out_len   Destination buffer length.
 * @param[out] req_len   Required payload length in bytes (optional).
 */
esp_err_t esp_storage_lfs_read(uint16_t slot, void *out, size_t out_len, size_t *req_len);

/**
 * @brief Get payload size of LittleFS slot [0..255].
 */
esp_err_t esp_storage_lfs_get_size(uint16_t slot, size_t *out_size);

/**
 * @brief Check whether LittleFS slot contains active data.
 *
 * Active means the backing file exists, even if it is empty.
 */
esp_err_t esp_storage_lfs_is_active(uint16_t slot, bool *out_active);

/**
 * @brief Erase LittleFS slot file.
 */
esp_err_t esp_storage_lfs_erase(uint16_t slot);

/**
 * @brief Write text payload (including JSON text) to LittleFS slot [0..255].
 *
 * Stored without trailing '\0'. Use esp_storage_lfs_read_string() to recover.
 */
esp_err_t esp_storage_lfs_write_string(uint16_t slot, const char *text);

/**
 * @brief Read text payload from LittleFS slot [0..255].
 *
 * Ensures trailing '\0' in out buffer.
 */
esp_err_t esp_storage_lfs_read_string(uint16_t slot, char *out, size_t out_len, size_t *req_len);

/**
 * @brief Alias for JSON text write.
 */
esp_err_t esp_storage_lfs_write_json(uint16_t slot, const char *json);

/**
 * @brief Alias for JSON text read.
 */
esp_err_t esp_storage_lfs_read_json(uint16_t slot, char *out, size_t out_len, size_t *req_len);

#ifdef __cplusplus
}
#endif
