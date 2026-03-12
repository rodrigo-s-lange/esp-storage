idf_component_register(
    SRCS "esp_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash littlefs esp_at
)
