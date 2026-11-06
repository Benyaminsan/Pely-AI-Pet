idf_component_register(
    SRCS "camera_hal.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos
    PRIV_REQUIRES esp_driver_gpio heap
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
