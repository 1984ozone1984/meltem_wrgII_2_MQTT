idf_component_register(
    SRCS "config_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server config_manager wifi_manager esp_system wrg2_driver app_update
)
