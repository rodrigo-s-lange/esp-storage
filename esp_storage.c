#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_littlefs.h"
#include "esp_log.h"

#include "esp_at.h"
#include "esp_storage.h"

#define STORAGE_NVS_NAMESPACE         "storage"
#define STORAGE_LFS_BASE_PATH         "/littlefs"
#define STORAGE_LFS_PARTITION_LABEL   "storage"
#define STORAGE_SLOT_MAX              255U
#define STORAGE_AT_MAX_READ_LEN        1024U

typedef struct {
    bool mounted;
    bool initialized;
    bool log_enabled;
    bool at_enabled;
    bool at_cmds_registered;
    SemaphoreHandle_t lock;
} storage_state_t;

static const char *TAG = "esp_storage";
static storage_state_t s_state = {0};

static bool s_nvs_registered  = false;
static bool s_lfs_registered  = false;
static bool s_slots_registered = false;

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

static char *_trim_ws(char *s)
{
    if (s == NULL) return NULL;
    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
    return s;
}

static bool _eq_ci_char(char a, char b)
{
    return (char)toupper((unsigned char)a) == (char)toupper((unsigned char)b);
}

static bool _is_null_literal(const char *value)
{
    if (value == NULL) return false;
    return strlen(value) == 4U &&
           _eq_ci_char(value[0], 'N') &&
           _eq_ci_char(value[1], 'U') &&
           _eq_ci_char(value[2], 'L') &&
           _eq_ci_char(value[3], 'L');
}

static bool _buffer_is_text(const uint8_t *data, size_t len)
{
    if (data == NULL) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = data[i];
        if (c == '\r' || c == '\n' || c == '\t') continue;
        if (c < 32 || c > 126) return false;
    }
    return true;
}

static void _nvs_make_keys(uint16_t slot, char *k_type, size_t k_type_len,
                           char *k_int, size_t k_int_len,
                           char *k_str, size_t k_str_len,
                           char *k_num, size_t k_num_len)
{
    snprintf(k_type, k_type_len, "s%03u_t", (unsigned)slot);
    snprintf(k_int,  k_int_len,  "s%03u_i", (unsigned)slot);
    snprintf(k_str,  k_str_len,  "s%03u_s", (unsigned)slot);
    if (k_num != NULL && k_num_len > 0U) {
        snprintf(k_num,  k_num_len,  "s%03u_n", (unsigned)slot);
    }
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

static esp_err_t _parse_slot_and_value(const char *param,
                                       uint16_t *out_slot,
                                       bool *out_has_value,
                                       char **out_value,
                                       bool *out_quoted,
                                       char *work,
                                       size_t work_len)
{
    if (param == NULL || out_slot == NULL || out_has_value == NULL ||
        out_value == NULL || work == NULL || work_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(work, param, work_len - 1U);
    work[work_len - 1U] = '\0';

    char *p = _trim_ws(work);
    if (p[0] == '\0') return ESP_ERR_INVALID_ARG;

    char *comma = strchr(p, ',');
    char *value = NULL;
    bool has_value = false;
    bool quoted = false;

    if (comma != NULL) {
        *comma = '\0';
        value = _trim_ws(comma + 1);
        has_value = true;

        size_t vlen = strlen(value);
        if (vlen >= 2 && value[0] == '"' && value[vlen - 1] == '"') {
            value[vlen - 1] = '\0';
            value++;
            quoted = true;
        }
    }

    char *slot_str = _trim_ws(p);

    errno = 0;
    char *end = NULL;
    long slot_l = strtol(slot_str, &end, 10);
    if (errno != 0 || end == slot_str || *end != '\0' || slot_l < 0 || slot_l > STORAGE_SLOT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_slot = (uint16_t)slot_l;
    *out_has_value = has_value;
    *out_value = value;
    if (out_quoted != NULL) *out_quoted = quoted;
    return ESP_OK;
}

static void _at_report_storage_err(const char *op, esp_err_t err)
{
    AT(R "ERROR: %s (%s)", op, esp_err_to_name(err));
}

static void _at_handle_slots(const char *param)
{
    bool list_nvs = true;
    bool list_lfs = true;

    if (param != NULL && param[0] != '\0') {
        if (_eq_ci_char(param[0], 'N') &&
            _eq_ci_char(param[1], 'V') &&
            _eq_ci_char(param[2], 'S') &&
            param[3] == '\0') {
            list_lfs = false;
        } else if (_eq_ci_char(param[0], 'L') &&
                   _eq_ci_char(param[1], 'F') &&
                   _eq_ci_char(param[2], 'S') &&
                   param[3] == '\0') {
            list_nvs = false;
        } else {
            AT(Y "Uso: AT+SLOTS?[=NVS|LFS]");
            return;
        }
    }

    bool found = false;
    AT(G "Slots ativos:");

    if (list_nvs) {
        for (uint16_t slot = 0; slot <= STORAGE_SLOT_MAX; slot++) {
            bool active = false;
            esp_err_t err = esp_storage_nvs_is_active(slot, &active);
            if (err != ESP_OK) {
                _at_report_storage_err("nvs is active", err);
                return;
            }
            if (!active) continue;

            esp_storage_nvs_type_t type = ESP_STORAGE_NVS_EMPTY;
            err = esp_storage_nvs_get_type(slot, &type);
            if (err != ESP_OK) {
                _at_report_storage_err("nvs get type", err);
                return;
            }

            found = true;
            if (type == ESP_STORAGE_NVS_INT) {
                int64_t value = 0;
                err = esp_storage_nvs_get_int(slot, &value);
                if (err != ESP_OK) {
                    _at_report_storage_err("nvs get int", err);
                    return;
                }
                AT(O "NVS[%u]=" C "%lld", (unsigned)slot, (long long)value);
                continue;
            }

            if (type == ESP_STORAGE_NVS_NUMBER) {
                double value = 0.0;
                err = esp_storage_nvs_get_number(slot, &value);
                if (err != ESP_OK) {
                    _at_report_storage_err("nvs get number", err);
                    return;
                }
                AT(O "NVS[%u]=" C "%.15g", (unsigned)slot, value);
                continue;
            }

            if (type == ESP_STORAGE_NVS_STRING) {
                size_t need = 0;
                err = esp_storage_nvs_get_string(slot, NULL, 0, &need);
                if (err != ESP_OK) {
                    _at_report_storage_err("nvs get string", err);
                    return;
                }
                if (need > STORAGE_AT_MAX_READ_LEN + 1U) {
                    AT(O "NVS[%u]=" Y "\"<%u bytes>\"", (unsigned)slot, (unsigned)(need - 1U));
                    continue;
                }

                char buf[STORAGE_AT_MAX_READ_LEN + 1U];
                err = esp_storage_nvs_get_string(slot, buf, need, NULL);
                if (err != ESP_OK) {
                    _at_report_storage_err("nvs get string", err);
                    return;
                }
                AT(O "NVS[%u]=" W "\"%s\"", (unsigned)slot, buf);
            }
        }
    }

    if (list_lfs) {
        for (uint16_t slot = 0; slot <= STORAGE_SLOT_MAX; slot++) {
            bool active = false;
            esp_err_t err = esp_storage_lfs_is_active(slot, &active);
            if (err != ESP_OK) {
                _at_report_storage_err("lfs is active", err);
                return;
            }
            if (!active) continue;

            size_t size = 0;
            err = esp_storage_lfs_get_size(slot, &size);
            if (err != ESP_OK) {
                _at_report_storage_err("lfs get size", err);
                return;
            }

            found = true;
            if (size > STORAGE_AT_MAX_READ_LEN) {
                AT(O "LFS[%u]=" Y "<%u bytes>", (unsigned)slot, (unsigned)size);
                continue;
            }

            if (size == 0U) {
                AT(O "LFS[%u]=" W, (unsigned)slot);
                continue;
            }

            uint8_t buf[STORAGE_AT_MAX_READ_LEN + 1U];
            memset(buf, 0, sizeof(buf));
            err = esp_storage_lfs_read(slot, buf, size, NULL);
            if (err != ESP_OK) {
                _at_report_storage_err("lfs read", err);
                return;
            }

            if (_buffer_is_text(buf, size)) {
                buf[size] = '\0';
                AT(O "LFS[%u]=" W "%s", (unsigned)slot, (char *)buf);
            } else {
                AT(O "LFS[%u]=" Y "<%u bytes binarios>", (unsigned)slot, (unsigned)size);
            }
        }
    }

    if (!found) {
        AT(Y "Nenhum slot ativo");
    }
}

static void _at_handle_nvs(const char *param)
{
    char work[256];
    uint16_t slot = 0;
    bool has_value = false;
    char *value = NULL;
    bool quoted = false;

    esp_err_t err = _parse_slot_and_value(param, &slot, &has_value, &value, &quoted, work, sizeof(work));
    if (err != ESP_OK) {
        AT(Y "Uso: AT+NVS=<slot>[,<numero|\"texto\"|NULL>]");
        return;
    }

    if (has_value) {
        if (_is_null_literal(value)) {
            err = esp_storage_nvs_erase(slot);
            if (err != ESP_OK) {
                _at_report_storage_err("nvs erase", err);
                return;
            }
            AT(G "OK");
            return;
        }

        if (quoted) {
            if (value == NULL) value = "";
            err = esp_storage_nvs_set_string(slot, value);
            if (err != ESP_OK) {
                _at_report_storage_err("nvs set string", err);
                return;
            }
            AT(G "OK");
            return;
        }

        if (value == NULL || value[0] == '\0') {
            AT(R "ERROR: valor vazio");
            return;
        }

        if (strchr(value, '.') == NULL && strchr(value, 'e') == NULL && strchr(value, 'E') == NULL) {
            errno = 0;
            char *end = NULL;
            long long v = strtoll(value, &end, 10);
            if (errno != 0 || end == value || *end != '\0') {
                AT(R "ERROR: numero invalido");
                return;
            }

            err = esp_storage_nvs_set_int(slot, (int64_t)v);
            if (err != ESP_OK) {
                _at_report_storage_err("nvs set int", err);
                return;
            }
            AT(G "OK");
            return;
        }

        errno = 0;
        char *end = NULL;
        double v = strtod(value, &end);
        if (errno != 0 || end == value || *end != '\0') {
            AT(R "ERROR: numero invalido");
            return;
        }

        err = esp_storage_nvs_set_number(slot, v);
        if (err != ESP_OK) {
            _at_report_storage_err("nvs set number", err);
            return;
        }
        AT(G "OK");
        return;
    }

    esp_storage_nvs_type_t type = ESP_STORAGE_NVS_EMPTY;
    err = esp_storage_nvs_get_type(slot, &type);
    if (err != ESP_OK || type == ESP_STORAGE_NVS_EMPTY) {
        _at_report_storage_err("nvs get type", (err == ESP_OK) ? ESP_ERR_NOT_FOUND : err);
        return;
    }

    if (type == ESP_STORAGE_NVS_INT) {
        int64_t out = 0;
        err = esp_storage_nvs_get_int(slot, &out);
        if (err != ESP_OK) {
            _at_report_storage_err("nvs get int", err);
            return;
        }
        AT(O "NVS[%u]=" C "%lld", (unsigned)slot, (long long)out);
        return;
    }

    if (type == ESP_STORAGE_NVS_NUMBER) {
        double out = 0.0;
        err = esp_storage_nvs_get_number(slot, &out);
        if (err != ESP_OK) {
            _at_report_storage_err("nvs get number", err);
            return;
        }
        AT(O "NVS[%u]=" C "%.15g", (unsigned)slot, out);
        return;
    }

    size_t need = 0;
    err = esp_storage_nvs_get_string(slot, NULL, 0, &need);
    if (err != ESP_OK) {
        _at_report_storage_err("nvs get string", err);
        return;
    }
    if (need > STORAGE_AT_MAX_READ_LEN + 1U) {
        AT(O "NVS[%u]=" Y "\"<%u bytes>\"", (unsigned)slot, (unsigned)(need - 1U));
        return;
    }

    char buf[STORAGE_AT_MAX_READ_LEN + 1U];
    err = esp_storage_nvs_get_string(slot, buf, need, NULL);
    if (err != ESP_OK) {
        _at_report_storage_err("nvs get string", err);
        return;
    }
    AT(O "NVS[%u]=" W "\"%s\"", (unsigned)slot, buf);
}

static void _at_handle_lfs(const char *param)
{
    char work[256];
    uint16_t slot = 0;
    bool has_value = false;
    char *value = NULL;

    esp_err_t err = _parse_slot_and_value(param, &slot, &has_value, &value, NULL, work, sizeof(work));
    if (err != ESP_OK) {
        AT(Y "Uso: AT+LFS=<slot>[,<texto/json>]");
        return;
    }

    if (has_value) {
        if (_is_null_literal(value)) {
            err = esp_storage_lfs_erase(slot);
            if (err != ESP_OK) {
                _at_report_storage_err("lfs erase", err);
                return;
            }
            AT(G "OK");
            return;
        }
        if (value == NULL) value = "";
        err = esp_storage_lfs_write_string(slot, value);
        if (err != ESP_OK) {
            _at_report_storage_err("lfs write", err);
            return;
        }
        AT(G "OK");
        return;
    }

    size_t need = 0;
    err = esp_storage_lfs_read_string(slot, NULL, 0, &need);
    if (err != ESP_OK) {
        _at_report_storage_err("lfs read", err);
        return;
    }
    size_t payload_len = (need > 0U) ? (need - 1U) : 0U;
    if (payload_len > STORAGE_AT_MAX_READ_LEN) {
        AT(Y "LFS[%u] muito grande (%u bytes). Limite AT=%u",
           (unsigned)slot, (unsigned)payload_len, (unsigned)STORAGE_AT_MAX_READ_LEN);
        return;
    }

    char *buf = calloc(1, need);
    if (buf == NULL) {
        _at_report_storage_err("alloc", ESP_ERR_NO_MEM);
        return;
    }
    err = esp_storage_lfs_read_string(slot, buf, need, NULL);
    if (err != ESP_OK) {
        free(buf);
        _at_report_storage_err("lfs read", err);
        return;
    }
    AT(O "LFS[%u]=" W "%s", (unsigned)slot, buf);
    free(buf);
}

static esp_err_t _register_at_commands(void)
{
    esp_err_t err = ESP_OK;

    if (!s_nvs_registered) {
        err = esp_at_register_cmd_example("AT+NVS", _at_handle_nvs, "AT+NVS=0,99999.5 ou AT+NVS=1,\"texto\" ou AT+NVS=2,NULL");
        if (err != ESP_OK) return err;
        s_nvs_registered = true;
    }

    if (!s_lfs_registered) {
        err = esp_at_register_cmd_example("AT+LFS", _at_handle_lfs, "AT+LFS=10,{\"key\":\"value\"} ou AT+LFS=10,\"texto\" ou AT+LFS=10,NULL");
        if (err != ESP_OK) return err;
        s_lfs_registered = true;
    }

    if (!s_slots_registered) {
        err = esp_at_register_cmd_example("AT+SLOTS?", _at_handle_slots, "AT+SLOTS?=NVS ou AT+SLOTS?=LFS ou AT+SLOTS?");
        if (err != ESP_OK) return err;
        s_slots_registered = true;
    }
    err = esp_at_set_help_visible("AT+NVS", false);
    if (err != ESP_OK) return err;
    err = esp_at_set_help_visible("AT+LFS", false);
    if (err != ESP_OK) return err;

    s_state.at_cmds_registered = true;
    return ESP_OK;
}

static void _unregister_at_commands(void)
{
    if (s_nvs_registered) {
        (void)esp_at_unregister_cmd("AT+NVS");
        s_nvs_registered = false;
    }
    if (s_lfs_registered) {
        (void)esp_at_unregister_cmd("AT+LFS");
        s_lfs_registered = false;
    }
    if (s_slots_registered) {
        (void)esp_at_unregister_cmd("AT+SLOTS?");
        s_slots_registered = false;
    }
    s_state.at_cmds_registered = false;
}

static esp_err_t _nvs_get_type_internal(nvs_handle_t h, uint16_t slot, esp_storage_nvs_type_t *out_type)
{
    char k_type[8];
    unsigned slot_u8 = (unsigned)(slot & 0xFFU);
    uint8_t type_u8 = 0;
    snprintf(k_type, sizeof(k_type), "s%03u_t", slot_u8);

    esp_err_t err = nvs_get_u8(h, k_type, &type_u8);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out_type = ESP_STORAGE_NVS_EMPTY;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    if (type_u8 > ESP_STORAGE_NVS_NUMBER) {
        *out_type = ESP_STORAGE_NVS_EMPTY;
        return ESP_OK;
    }

    *out_type = (esp_storage_nvs_type_t)type_u8;
    return ESP_OK;
}

esp_err_t esp_storage_init(bool log_enabled, bool at_enabled)
{
    if (s_state.initialized) return ESP_ERR_INVALID_STATE;

    s_state.log_enabled = log_enabled;
    s_state.at_enabled = at_enabled;
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
        if (s_state.log_enabled) {
            ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        }
        return err;
    }

    s_state.mounted = true;

    if (s_state.at_enabled) {
        err = _register_at_commands();
        if (err != ESP_OK) {
            esp_vfs_littlefs_unregister(STORAGE_LFS_PARTITION_LABEL);
            s_state.mounted = false;
            vSemaphoreDelete(s_state.lock);
            s_state.lock = NULL;
            if (s_state.log_enabled) {
                ESP_LOGE(TAG, "AT commands register failed: %s", esp_err_to_name(err));
            }
            return err;
        }
    }

    s_state.initialized = true;
    if (s_state.log_enabled) {
        if (s_state.at_enabled) {
            ESP_LOGI(TAG, "initialized (nvs=%s lfs=%s, AT cmds=AT+NVS/AT+LFS/AT+SLOTS?)",
                     STORAGE_NVS_NAMESPACE, STORAGE_LFS_BASE_PATH);
        } else {
            ESP_LOGI(TAG, "initialized (nvs=%s lfs=%s, AT disabled)",
                     STORAGE_NVS_NAMESPACE, STORAGE_LFS_BASE_PATH);
        }
    }
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
    if (s_state.at_cmds_registered) {
        _unregister_at_commands();
    }

    xSemaphoreGive(s_state.lock);
    vSemaphoreDelete(s_state.lock);
    s_state.lock = NULL;
    s_state.log_enabled = false;
    s_state.at_enabled = false;

    return err;
}

bool esp_storage_is_initialized(void)
{
    return s_state.initialized;
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

esp_err_t esp_storage_nvs_is_active(uint16_t slot, bool *out_active)
{
    if (!_slot_valid(slot) || out_active == NULL) return ESP_ERR_INVALID_ARG;

    esp_storage_nvs_type_t type = ESP_STORAGE_NVS_EMPTY;
    esp_err_t err = esp_storage_nvs_get_type(slot, &type);
    if (err != ESP_OK) return err;

    *out_active = (type != ESP_STORAGE_NVS_EMPTY);
    return ESP_OK;
}

esp_err_t esp_storage_nvs_set_int(uint16_t slot, int64_t value)
{
    if (!_slot_valid(slot)) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    char k_type[8];
    char k_int[8];
    char k_str[8];
    char k_num[8];
    _nvs_make_keys(slot, k_type, sizeof(k_type), k_int, sizeof(k_int), k_str, sizeof(k_str), k_num, sizeof(k_num));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    nvs_handle_t h = 0;
    esp_err_t err = _nvs_open_rw(&h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_state.lock);
        return err;
    }

    nvs_erase_key(h, k_str);
    nvs_erase_key(h, k_num);
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
    char k_num[8];
    _nvs_make_keys(slot, k_type, sizeof(k_type), k_int, sizeof(k_int), k_str, sizeof(k_str), k_num, sizeof(k_num));

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

esp_err_t esp_storage_nvs_set_number(uint16_t slot, double value)
{
    if (!_slot_valid(slot)) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    char k_type[8];
    char k_int[8];
    char k_str[8];
    char k_num[8];
    _nvs_make_keys(slot, k_type, sizeof(k_type), k_int, sizeof(k_int), k_str, sizeof(k_str), k_num, sizeof(k_num));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    nvs_handle_t h = 0;
    esp_err_t err = _nvs_open_rw(&h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_state.lock);
        return err;
    }

    nvs_erase_key(h, k_int);
    nvs_erase_key(h, k_str);
    err = nvs_set_blob(h, k_num, &value, sizeof(value));
    if (err == ESP_OK) err = nvs_set_u8(h, k_type, ESP_STORAGE_NVS_NUMBER);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    xSemaphoreGive(s_state.lock);
    return err;
}

esp_err_t esp_storage_nvs_get_number(uint16_t slot, double *out_value)
{
    if (!_slot_valid(slot) || out_value == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    char k_type[8];
    char k_int[8];
    char k_str[8];
    char k_num[8];
    _nvs_make_keys(slot, k_type, sizeof(k_type), k_int, sizeof(k_int), k_str, sizeof(k_str), k_num, sizeof(k_num));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    nvs_handle_t h = 0;
    esp_err_t err = _nvs_open_ro(&h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_state.lock);
        return err;
    }

    esp_storage_nvs_type_t type = ESP_STORAGE_NVS_EMPTY;
    err = _nvs_get_type_internal(h, slot, &type);
    if (err == ESP_OK && type != ESP_STORAGE_NVS_NUMBER) {
        err = (type == ESP_STORAGE_NVS_EMPTY) ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        size_t len = sizeof(*out_value);
        err = nvs_get_blob(h, k_num, out_value, &len);
        if (err == ESP_OK && len != sizeof(*out_value)) {
            err = ESP_ERR_INVALID_SIZE;
        }
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
    char k_num[8];
    _nvs_make_keys(slot, k_type, sizeof(k_type), k_int, sizeof(k_int), k_str, sizeof(k_str), k_num, sizeof(k_num));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    nvs_handle_t h = 0;
    esp_err_t err = _nvs_open_rw(&h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_state.lock);
        return err;
    }

    nvs_erase_key(h, k_int);
    nvs_erase_key(h, k_num);
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
    if (out == NULL && req_len == NULL) return ESP_ERR_INVALID_ARG;
    if (out != NULL && out_len == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    char k_type[8];
    char k_int[8];
    char k_str[8];
    char k_num[8];
    _nvs_make_keys(slot, k_type, sizeof(k_type), k_int, sizeof(k_int), k_str, sizeof(k_str), k_num, sizeof(k_num));

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
    if (err == ESP_OK && out == NULL) {
        nvs_close(h);
        xSemaphoreGive(s_state.lock);
        return ESP_OK;
    }
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
    char k_num[8];
    _nvs_make_keys(slot, k_type, sizeof(k_type), k_int, sizeof(k_int), k_str, sizeof(k_str), k_num, sizeof(k_num));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    nvs_handle_t h = 0;
    esp_err_t err = _nvs_open_rw(&h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_state.lock);
        return err;
    }

    nvs_erase_key(h, k_int);
    nvs_erase_key(h, k_str);
    nvs_erase_key(h, k_num);
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

esp_err_t esp_storage_lfs_is_active(uint16_t slot, bool *out_active)
{
    if (!_slot_valid(slot) || out_active == NULL) return ESP_ERR_INVALID_ARG;

    size_t size = 0;
    esp_err_t err = esp_storage_lfs_get_size(slot, &size);
    if (err == ESP_ERR_NOT_FOUND) {
        *out_active = false;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    *out_active = true;
    return ESP_OK;
}

esp_err_t esp_storage_lfs_read(uint16_t slot, void *out, size_t out_len, size_t *req_len)
{
    if (!_slot_valid(slot)) return ESP_ERR_INVALID_ARG;
    if (out == NULL && req_len == NULL) return ESP_ERR_INVALID_ARG;
    if (out != NULL && out_len == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    char path[48];
    _lfs_make_path(slot, path, sizeof(path));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        xSemaphoreGive(s_state.lock);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0L, SEEK_END) != 0) {
        fclose(f);
        xSemaphoreGive(s_state.lock);
        return ESP_FAIL;
    }
    long end = ftell(f);
    if (end < 0) {
        fclose(f);
        xSemaphoreGive(s_state.lock);
        return ESP_FAIL;
    }
    if (fseek(f, 0L, SEEK_SET) != 0) {
        fclose(f);
        xSemaphoreGive(s_state.lock);
        return ESP_FAIL;
    }

    size_t need = (size_t)end;
    if (req_len != NULL) *req_len = need;
    if (out == NULL) {
        fclose(f);
        xSemaphoreGive(s_state.lock);
        return ESP_OK;
    }
    if (out_len < need) {
        fclose(f);
        xSemaphoreGive(s_state.lock);
        return ESP_ERR_INVALID_SIZE;
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
    if (!_slot_valid(slot)) return ESP_ERR_INVALID_ARG;
    if (out == NULL && req_len == NULL) return ESP_ERR_INVALID_ARG;
    if (out != NULL && out_len == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t ready = _check_ready();
    if (ready != ESP_OK) return ready;

    char path[48];
    _lfs_make_path(slot, path, sizeof(path));

    xSemaphoreTake(s_state.lock, portMAX_DELAY);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        xSemaphoreGive(s_state.lock);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0L, SEEK_END) != 0) {
        fclose(f);
        xSemaphoreGive(s_state.lock);
        return ESP_FAIL;
    }
    long end = ftell(f);
    if (end < 0) {
        fclose(f);
        xSemaphoreGive(s_state.lock);
        return ESP_FAIL;
    }
    if (fseek(f, 0L, SEEK_SET) != 0) {
        fclose(f);
        xSemaphoreGive(s_state.lock);
        return ESP_FAIL;
    }

    size_t payload_len = (size_t)end;
    size_t need = payload_len + 1U;
    if (req_len != NULL) *req_len = need;
    if (out == NULL) {
        fclose(f);
        xSemaphoreGive(s_state.lock);
        return ESP_OK;
    }
    if (out_len < need) {
        fclose(f);
        xSemaphoreGive(s_state.lock);
        return ESP_ERR_INVALID_SIZE;
    }
    if (payload_len > 0 && fread(out, 1, payload_len, f) != payload_len) {
        fclose(f);
        xSemaphoreGive(s_state.lock);
        return ESP_FAIL;
    }
    out[payload_len] = '\0';

    fclose(f);
    xSemaphoreGive(s_state.lock);
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
