idf_component_register(
    SRCS "cloud_sync.cpp"
    INCLUDE_DIRS "include"
    REQUIRES pet_logic esp_event freertos nvs_flash
    PRIV_REQUIRES esp_wifi esp_netif esp_timer esp_hw_support json
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
