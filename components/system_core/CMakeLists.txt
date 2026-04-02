idf_component_register(
    SRCS "system_core.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_system nvs_flash
)
