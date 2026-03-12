# esp_storage - Examples

Full C example file:
- `examples/esp_storage_example_main.c`

## 1) Basic initialization

```c
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_storage.h"

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_storage_init());
}
```

## 2) NVS slot as integer

```c
int64_t value = 0;

ESP_ERROR_CHECK(esp_storage_nvs_set_int(0, 25));
ESP_ERROR_CHECK(esp_storage_nvs_get_int(0, &value));
```

## 3) NVS slot as string

```c
char buf[64];
size_t required = 0;

ESP_ERROR_CHECK(esp_storage_nvs_set_string(1, "string_exemplo"));
ESP_ERROR_CHECK(esp_storage_nvs_get_string(1, buf, sizeof(buf), &required));
```

## 4) LittleFS slot as binary blob

```c
uint8_t payload[] = {0xAA, 0xBB, 0xCC};
uint8_t out[8];
size_t need = 0;

ESP_ERROR_CHECK(esp_storage_lfs_write(10, payload, sizeof(payload)));
ESP_ERROR_CHECK(esp_storage_lfs_read(10, out, sizeof(out), &need));
```

## 5) LittleFS slot as JSON text

```c
const char *json = "{\"setpoint\":25,\"enabled\":true}";
char out_json[128];
size_t required = 0;

ESP_ERROR_CHECK(esp_storage_lfs_write_json(20, json));
ESP_ERROR_CHECK(esp_storage_lfs_read_json(20, out_json, sizeof(out_json), &required));
```

## 6) Slot erase

```c
ESP_ERROR_CHECK(esp_storage_nvs_erase(1));
ESP_ERROR_CHECK(esp_storage_lfs_erase(20));
```

## Notes

- Valid slot range is `0..255`.
- `esp_at_init()` must be called before `esp_storage_init()`.
- Serializer is not required at storage layer:
  - NVS stores typed int/string data.
  - LittleFS stores raw bytes directly.

## 7) Same operations via AT

```text
AT+NVSI=1,25
AT+NVSI=1

AT+NVSS=2,string_exemplo
AT+NVSS=2

AT+LFS=10,{"key":"value"}
AT+LFS=10
```

## 8) Recommended boot order

```c
nvs_flash_init();
esp_at_init();
esp_storage_init();
```
