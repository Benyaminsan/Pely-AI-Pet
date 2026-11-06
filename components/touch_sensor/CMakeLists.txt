idf_component_register(
    SRCS "touch_sensor.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_event freertos esp_common
    PRIV_REQUIRES driver esp_driver_gpio esp_timer esp_hw_support
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
