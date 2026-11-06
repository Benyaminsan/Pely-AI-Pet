idf_component_register(
    SRCS "vision_ai.cpp"
    INCLUDE_DIRS "include"
    REQUIRES camera_hal esp_event freertos
    PRIV_REQUIRES esp_timer heap
)
set(vision_model_dir "${CMAKE_CURRENT_LIST_DIR}/../../assets/model")
if(NOT EXISTS "${vision_model_dir}/fomo_model.h")
    message(WARNING
        "Configure hardware with menuconfig, then supply assets/model/fomo_model.h "
        "defining fomo_model_int8[] with a genuine ESP-DL export before building firmware.")
endif()
target_include_directories(${COMPONENT_LIB} PRIVATE "${vision_model_dir}")
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
