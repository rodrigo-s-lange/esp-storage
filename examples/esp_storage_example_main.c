#include <string.h>
#include <inttypes.h>
#include <stdlib.h>

#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_at.h"
#include "esp_storage.h"

/*
 * Example-only file.
 * Copy selected parts to your project main/app_main.
 */
void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_at_init());
    ESP_ERROR_CHECK(esp_storage_init());

    /* NVS int slot */
    ESP_ERROR_CHECK(esp_storage_nvs_set_int(1, 25));
    int64_t setpoint = 0;
    ESP_ERROR_CHECK(esp_storage_nvs_get_int(1, &setpoint));
    AT("NVS int slot 1 = %" PRId64, setpoint);

    /* NVS string slot */
    ESP_ERROR_CHECK(esp_storage_nvs_set_string(2, "string_exemplo"));
    size_t str_need = 0;
    ESP_ERROR_CHECK(esp_storage_nvs_get_string(2, NULL, 0, &str_need));
    char *str_buf = calloc(1, str_need);
    if (str_buf != NULL) {
        ESP_ERROR_CHECK(esp_storage_nvs_get_string(2, str_buf, str_need, NULL));
        AT("NVS str slot 2 = %s", str_buf);
        free(str_buf);
    }

    /* LittleFS JSON slot */
    const char *json = "{\"mode\":\"auto\",\"setpoint\":25}";
    ESP_ERROR_CHECK(esp_storage_lfs_write_json(10, json));

    size_t json_need = 0;
    ESP_ERROR_CHECK(esp_storage_lfs_read_json(10, NULL, 0, &json_need));
    char *json_buf = calloc(1, json_need);
    if (json_buf != NULL) {
        ESP_ERROR_CHECK(esp_storage_lfs_read_json(10, json_buf, json_need, NULL));
        AT("LFS json slot 10 = %s", json_buf);
        free(json_buf);
    }

    /*
     * Same data via AT:
     *   AT+NVSI=1
     *   AT+NVSS=2
     *   AT+LFS=10
     */
}
