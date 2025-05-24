idf_component_register(
    SRCS "tcp_server.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_eth esp_event esp_ringbuf tasks msp
)
