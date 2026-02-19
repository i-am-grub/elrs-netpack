idf_component_register(
    SRCS "espnow_server.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_app_format esp_netif nvs_flash esp_wifi esp_event esp_ringbuf tasks msp peer_manager
)
