# esp_storage

Slot-based storage component for ESP-IDF.

## Features

- Slots from `0` to `255`
- NVS slots:
  - integer (`int64_t`)
  - number (`double`)
  - string
- LittleFS slots:
  - raw binary (`blob`)
  - string
  - JSON text
- Optional AT commands for read/write and listing
- Optional internal logs

## Storage model

- NVS namespace: `storage`
- LittleFS mount point: `/littlefs`
- LittleFS file per slot: `/littlefs/slot_XXX.bin`

## Prerequisites

- `nvs_flash_init()` called before `esp_storage_init(...)`
- `esp_at_init(false)` called before `esp_storage_init(..., true)` when AT commands are desired
- Partition table containing a LittleFS partition labeled `storage`

## Init policy

`esp_storage_init(log_enabled, at_enabled)` controls optional behavior.

- `log_enabled`
  - enables storage logs
- `at_enabled`
  - registers `AT+NVS`, `AT+LFS` and `AT+SLOTS?`

## Usage

```c
#include <inttypes.h>
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_at.h"
#include "esp_storage.h"

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_at_init(false));
    ESP_ERROR_CHECK(esp_storage_init(false, true));

    ESP_ERROR_CHECK(esp_storage_nvs_set_int(1, 25));
    ESP_ERROR_CHECK(esp_storage_lfs_write_json(10, "{\"setpoint\":25,\"mode\":\"auto\"}"));
}
```

## AT commands

When `esp_storage_init(log_enabled, true)` is used:

- `AT+NVS=<slot>`
- `AT+NVS=<slot>,<int>`
- `AT+NVS=<slot>,<number>`
- `AT+NVS=<slot>,"<text>"`
- `AT+NVS=<slot>,NULL`
- `AT+LFS=<slot>`
- `AT+LFS=<slot>,<text_or_json>`
- `AT+LFS=<slot>,NULL`
- `AT+SLOTS?`
- `AT+SLOTS?=NVS`
- `AT+SLOTS?=LFS`

## Notes

- Public API is non-blocking apart from storage and filesystem operations themselves.
- Slot range is `0..255`.
- Slot exists independently of the payload value.
