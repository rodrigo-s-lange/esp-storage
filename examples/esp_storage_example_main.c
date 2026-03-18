#include <stdio.h>

#include "esp_at.h"
#include "esp_err.h"
#include "esp_storage.h"
#include "nvs_flash.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_at_init(false));
    ESP_ERROR_CHECK(esp_storage_init(false, true));

    ESP_ERROR_CHECK(esp_storage_nvs_set_int(1, 25));
    ESP_ERROR_CHECK(esp_storage_nvs_set_string(2, "string_exemplo"));
    ESP_ERROR_CHECK(esp_storage_nvs_set_number(3, 99999.5));
    ESP_ERROR_CHECK(esp_storage_lfs_write_json(10, "{\"setpoint\":25,\"mode\":\"auto\"}"));

    printf("esp_storage example ready\n");
}
