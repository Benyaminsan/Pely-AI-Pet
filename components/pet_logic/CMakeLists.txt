idf_component_register(
    SRCS "pet_logic.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_event freertos nvs_flash
    PRIV_REQUIRES esp_timer esp_hw_support
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
