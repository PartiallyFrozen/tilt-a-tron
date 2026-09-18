idf_component_register(
    SRCS "wifi.c" "ota_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif lwip esp_event nvs_flash esp_http_server app_update esp_system esp_timer esp_app_format
)
