#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_littlefs.h"
#include "esp_log.h"

#include "esp_storage.h"

#define STORAGE_NVS_NAMESPACE         "storage"
#define STORAGE_LFS_BASE_PATH         "/littlefs"
#define STORAGE_LFS_PARTITION_LABEL   "storage"
#define STORAGE_SLOT_MAX              255U

typedef struct {
    bool mounted;
    bool initialized;
    SemaphoreHandle_t lock;
} storage_state_t;

static const char *TAG = "esp_storage";
static storage_state_t s_state = {0};

static inline bool _slot_valid(uint16_t slot)
{
    return (slot <= STORAGE_SLOT_MAX);
}

static inline esp_err_t _check_ready(void)
{
    if (!s_state.initialized) return ESP_ERR_INVALID_STATE;
    if (s_state.lock == NULL) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

static void _nvs_make_keys(uint16_t slot, char *k_type, size_t k_type_len,
                           char *k_int, size_t k_int_len,
                           char *k_str, size_t k_str_len)
{
    snprintf(k_type, k_type_len, "s%03u_t", (unsigned)slot);
    snprintf(k_int,  k_int_len,  "s%03u_i", (unsigned)slot);
    snprintf(k_str,  k_str_len,  "s%03u_s", (unsigned)slot);
}

static void _lfs_make_path(uint16_t slot, char *path, size_t path_len)
{
    snprintf(path, path_len, STORAGE_LFS_BASE_PATH "/slot_%03u.bin", (unsigned)slot);
}

static esp_err_t _nvs_open_rw(nvs_handle_t *out)
{
    return nvs_open(STORAGE_NVS_NAMESPACE, NVS_READWRITE, out);
}

static esp_err_t _nvs_open_ro(nvs_handle_t *out)
{
    return nvs_open(STORAGE_NVS_NAMESPACE, NVS_READONLY, out);
}

static esp_err_t _nvs_get_type_internal(nvs_handle_t h, uint16_t slot, esp_storage_nvs_type_t *out_type)
{
    char k_type[8];
    char k_int[8];
    char k_str[8];
    uint8_t type_u8 = 0;
    _nvs_make_keys(slot, k_type, sizeof(k_type), k_int, sizeof(k_int), k_str, sizeof(k_str));

    esp_err_t err = nvs_get_u8(h, k_type, &type_u8);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out_type = ESP_STORAGE_NVS_EMPTY;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    if (type_u8 > ESP_STORAGE_NVS_STRING) {
        *out_type = ESP_STORAGE_NVS_EMPTY;
        return ESP_OK;
    }

    *out_type = (esp_storage_nvs_type_t)type_u8;
    return ESP_OK;
}

esp_err_t esp_storage_init(void)
{
    if (s_state.initialized) return ESP_ERR_INVALID_STATE;

    s_state.lock = xSemaphoreCreateMutex();
    if (s_state.lock == NULL) return ESP_ERR_NO_MEM;

    nvs_handle_t h = 0;
    esp_err_t err = _nvs_open_rw(&h);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_state.lock);
        s_state.lock = NULL;
        return err;
    }
    nvs_close(h);

    const esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_LFS_BASE_PATH,
        .partition_label = STORAGE_LFS_PARTITION_LABEL,
        .partition = NULL,
        .format_if_mount_failed = true,
        .dont_mount = false,
        .grow_on_mount = true,
    };

    err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_state.lock);
        s_state.lock = NULL;
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_state.mounted = true;
    s_state.initialized = true;
    ESP_LOGI(TAG, "initialized (nvs=%s lfs=%s)", STORAGE_NVS_NAMESPACE, STORAGE_LFS_BASE_PATH);
    return ESP_OK;
}

esp_err_t esp_storage_deinit(void)
{
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    xSemaphoreTake(s_state.lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    if (s_state.mounted) {
        err = esp_vfs_littlefs_unregister(STORAGE_LFS_PARTITION_LABEL);
        if (err == ESP_OK) {
            s_state.mounted = false;
        }
    }
    s_state.initialized = false;

    xSemaphoreGive(s_state.lock);
    vSemaphoreDelete(s_state.lock);
    s_state.lock = NULL;

    return err;
}

esp_err_t esp_storage_nvs_get_type(uint16_t slot, esp_storage_nvs_type_t *out_type)
{
    if (!_slot_valid(slot) || out_type == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    nvs_handle_t h = 0;
    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    esp_err_t err = _nvs_open_ro(&h);
    if (err == ESP_OK) {
        err = _nvs_get_type_internal(h, slot, out_type);
        nvs_close(h);
    }
    xSemaphoreGive(s_state.lock);
    return err;
}

esp_err_t esp_storage_nvs_set_int(uint16_t slot, int64_t value)
{
    if (!_slot_valid(slot)) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    char k_type[8];
    char k_int[8];
    char k_str[8];
    _nvs_make_keys(slot, k_type, sizeof(k_type), k_int, sizeof(k_int), k_str, sizeof(k_str));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    nvs_handle_t h = 0;
    esp_err_t err = _nvs_open_rw(&h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_state.lock);
        return err;
    }

    nvs_erase_key(h, k_str);
    err = nvs_set_i64(h, k_int, value);
    if (err == ESP_OK) err = nvs_set_u8(h, k_type, ESP_STORAGE_NVS_INT);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    xSemaphoreGive(s_state.lock);
    return err;
}

esp_err_t esp_storage_nvs_get_int(uint16_t slot, int64_t *out_value)
{
    if (!_slot_valid(slot) || out_value == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    char k_type[8];
    char k_int[8];
    char k_str[8];
    _nvs_make_keys(slot, k_type, sizeof(k_type), k_int, sizeof(k_int), k_str, sizeof(k_str));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    nvs_handle_t h = 0;
    esp_err_t err = _nvs_open_ro(&h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_state.lock);
        return err;
    }

    esp_storage_nvs_type_t type = ESP_STORAGE_NVS_EMPTY;
    err = _nvs_get_type_internal(h, slot, &type);
    if (err == ESP_OK && type != ESP_STORAGE_NVS_INT) {
        err = (type == ESP_STORAGE_NVS_EMPTY) ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        err = nvs_get_i64(h, k_int, out_value);
    }

    nvs_close(h);
    xSemaphoreGive(s_state.lock);
    return err;
}

esp_err_t esp_storage_nvs_set_string(uint16_t slot, const char *value)
{
    if (!_slot_valid(slot) || value == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    char k_type[8];
    char k_int[8];
    char k_str[8];
    _nvs_make_keys(slot, k_type, sizeof(k_type), k_int, sizeof(k_int), k_str, sizeof(k_str));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    nvs_handle_t h = 0;
    esp_err_t err = _nvs_open_rw(&h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_state.lock);
        return err;
    }

    nvs_erase_key(h, k_int);
    err = nvs_set_str(h, k_str, value);
    if (err == ESP_OK) err = nvs_set_u8(h, k_type, ESP_STORAGE_NVS_STRING);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    xSemaphoreGive(s_state.lock);
    return err;
}

esp_err_t esp_storage_nvs_get_string(uint16_t slot, char *out, size_t out_len, size_t *req_len)
{
    if (!_slot_valid(slot)) return ESP_ERR_INVALID_ARG;
    if (out == NULL || out_len == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    char k_type[8];
    char k_int[8];
    char k_str[8];
    _nvs_make_keys(slot, k_type, sizeof(k_type), k_int, sizeof(k_int), k_str, sizeof(k_str));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    nvs_handle_t h = 0;
    esp_err_t err = _nvs_open_ro(&h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_state.lock);
        return err;
    }

    esp_storage_nvs_type_t type = ESP_STORAGE_NVS_EMPTY;
    err = _nvs_get_type_internal(h, slot, &type);
    if (err == ESP_OK && type != ESP_STORAGE_NVS_STRING) {
        err = (type == ESP_STORAGE_NVS_EMPTY) ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_RESPONSE;
    }

    size_t need = 0;
    if (err == ESP_OK) {
        err = nvs_get_str(h, k_str, NULL, &need);
    }
    if (req_len != NULL) *req_len = need;
    if (err == ESP_OK && out_len < need) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        err = nvs_get_str(h, k_str, out, &out_len);
    }

    nvs_close(h);
    xSemaphoreGive(s_state.lock);
    return err;
}

esp_err_t esp_storage_nvs_erase(uint16_t slot)
{
    if (!_slot_valid(slot)) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    char k_type[8];
    char k_int[8];
    char k_str[8];
    _nvs_make_keys(slot, k_type, sizeof(k_type), k_int, sizeof(k_int), k_str, sizeof(k_str));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    nvs_handle_t h = 0;
    esp_err_t err = _nvs_open_rw(&h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_state.lock);
        return err;
    }

    nvs_erase_key(h, k_int);
    nvs_erase_key(h, k_str);
    nvs_erase_key(h, k_type);
    err = nvs_commit(h);

    nvs_close(h);
    xSemaphoreGive(s_state.lock);
    return err;
}

esp_err_t esp_storage_lfs_write(uint16_t slot, const void *data, size_t len)
{
    if (!_slot_valid(slot)) return ESP_ERR_INVALID_ARG;
    if (data == NULL && len > 0) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    char path[48];
    _lfs_make_path(slot, path, sizeof(path));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        xSemaphoreGive(s_state.lock);
        return ESP_FAIL;
    }

    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        xSemaphoreGive(s_state.lock);
        return ESP_FAIL;
    }

    fclose(f);
    xSemaphoreGive(s_state.lock);
    return ESP_OK;
}

esp_err_t esp_storage_lfs_get_size(uint16_t slot, size_t *out_size)
{
    if (!_slot_valid(slot) || out_size == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    char path[48];
    _lfs_make_path(slot, path, sizeof(path));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    struct stat st = {0};
    int rc = stat(path, &st);
    xSemaphoreGive(s_state.lock);
    if (rc != 0) return ESP_ERR_NOT_FOUND;

    *out_size = (size_t)st.st_size;
    return ESP_OK;
}

esp_err_t esp_storage_lfs_read(uint16_t slot, void *out, size_t out_len, size_t *req_len)
{
    if (!_slot_valid(slot) || out == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    size_t need = 0;
    esp_err_t err = esp_storage_lfs_get_size(slot, &need);
    if (req_len != NULL) *req_len = need;
    if (err != ESP_OK) return err;
    if (out_len < need) return ESP_ERR_INVALID_SIZE;

    char path[48];
    _lfs_make_path(slot, path, sizeof(path));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        xSemaphoreGive(s_state.lock);
        return ESP_FAIL;
    }

    if (need > 0 && fread(out, 1, need, f) != need) {
        fclose(f);
        xSemaphoreGive(s_state.lock);
        return ESP_FAIL;
    }

    fclose(f);
    xSemaphoreGive(s_state.lock);
    return ESP_OK;
}

esp_err_t esp_storage_lfs_erase(uint16_t slot)
{
    if (!_slot_valid(slot)) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    char path[48];
    _lfs_make_path(slot, path, sizeof(path));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    int rc = remove(path);
    xSemaphoreGive(s_state.lock);
    if (rc != 0) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

esp_err_t esp_storage_lfs_write_string(uint16_t slot, const char *text)
{
    if (text == NULL) return ESP_ERR_INVALID_ARG;
    return esp_storage_lfs_write(slot, text, strlen(text));
}

esp_err_t esp_storage_lfs_read_string(uint16_t slot, char *out, size_t out_len, size_t *req_len)
{
    if (out == NULL || out_len == 0) return ESP_ERR_INVALID_ARG;

    size_t need = 0;
    esp_err_t err = esp_storage_lfs_get_size(slot, &need);
    if (req_len != NULL) {
        *req_len = need + 1U;
    }
    if (err != ESP_OK) return err;
    if (out_len < (need + 1U)) return ESP_ERR_INVALID_SIZE;

    err = esp_storage_lfs_read(slot, out, need, NULL);
    if (err != ESP_OK) return err;
    out[need] = '\0';
    return ESP_OK;
}

esp_err_t esp_storage_lfs_write_json(uint16_t slot, const char *json)
{
    return esp_storage_lfs_write_string(slot, json);
}

esp_err_t esp_storage_lfs_read_json(uint16_t slot, char *out, size_t out_len, size_t *req_len)
{
    return esp_storage_lfs_read_string(slot, out, out_len, req_len);
}
