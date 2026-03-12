# esp_storage

Slot-based storage component for ESP-IDF.

## Features

- Slots from `0` to `255`
- NVS slots:
  - integer (`int64_t`)
  - string
- LittleFS slots:
  - raw binary (`blob`)
  - string
  - JSON text

## Storage model

- NVS namespace: `storage`
- NVS keys per slot:
  - `sXXX_t` => type (`empty`, `int`, `string`)
  - `sXXX_i` => integer payload
  - `sXXX_s` => string payload
- LittleFS file per slot:
  - `/littlefs/slot_XXX.bin`

## Serializer / deserializer

For this design, serializer is **not required**:
- NVS already stores native integer/string types.
- LittleFS stores raw bytes directly (including JSON text as plain bytes).

## Prerequisites

- `nvs_flash_init()` called before `esp_storage_init()`.
- `esp_at_init()` called before `esp_storage_init()` (AT commands are registered in init).
- Partition table containing a LittleFS partition labeled `storage`.

## Usage

```c
#include <inttypes.h>
#include "esp_err.h"
#include "esp_storage.h"

void app_main(void)
{
    ESP_ERROR_CHECK(esp_storage_init());

    // NVS slot: int
    ESP_ERROR_CHECK(esp_storage_nvs_set_int(1, 25));
    int64_t sp = 0;
    ESP_ERROR_CHECK(esp_storage_nvs_get_int(1, &sp));

    // NVS slot: string
    ESP_ERROR_CHECK(esp_storage_nvs_set_string(2, "string_exemplo"));

    // LittleFS slot: JSON text
ESP_ERROR_CHECK(esp_storage_lfs_write_json(10, "{\"setpoint\":25,\"mode\":\"auto\"}"));
}
```

See `EXAMPLES.md` for more complete usage snippets.

## AT commands (simplified read/write)

`esp_storage_init()` registers these commands:

- `AT+NVSI=<slot>` -> read NVS int slot
- `AT+NVSI=<slot>,<int64>` -> write NVS int slot
- `AT+NVSS=<slot>` -> read NVS string slot
- `AT+NVSS=<slot>,<text>` -> write NVS string slot
- `AT+LFS=<slot>` -> read LittleFS text slot
- `AT+LFS=<slot>,<text_or_json>` -> write LittleFS text slot

Examples:

```text
AT+NVSI=1,25
AT+NVSI=1
AT+NVSS=2,string_exemplo
AT+NVSS=2
AT+LFS=10,{"mode":"auto","setpoint":25}
AT+LFS=10
```

## Install

```bash
idf.py add-dependency "rodrigo-s-lange/esp_storage>=0.1.0"
```

## Targets

`esp32` - `esp32-c3` - `esp32-c6` - `esp32-s3`

## License

MIT - Copyright (c) 2026
