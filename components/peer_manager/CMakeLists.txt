idf_component_register(
    SRCS "peer_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_ringbuf msp
)
