idf_component_register(
    SRCS "led_matrix.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos
    PRIV_REQUIRES esp_driver_gpio esp_timer esp_hw_support heap
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
