idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES pet_logic vision_ai camera_hal led_matrix touch_sensor cloud_sync
                  nvs_flash freertos esp_event esp_netif esp_wifi esp_driver_gpio
                  esp_hw_support esp_timer json
    EMBED_TXTFILES "../data/food_catalog.json"
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
