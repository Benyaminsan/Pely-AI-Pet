#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>

#include "camera_hal.hpp"
#include "cJSON.h"
#include "led_matrix.hpp"
#include "pet_logic.hpp"
#include "sdkconfig.h"
#include "touch_sensor.hpp"
#include "vision_ai.hpp"

namespace pely {
// Own the parsed JSON for the firmware lifetime; component interfaces borrow its strings.
class FoodCatalog final {
public:
    ~FoodCatalog() { cJSON_Delete(root_); }
    FoodCatalog() = default;
    FoodCatalog(const FoodCatalog&) = delete;
    FoodCatalog& operator=(const FoodCatalog&) = delete;

    esp_err_t load(const char* json) {
        if (root_) { return ESP_ERR_INVALID_STATE; }
        root_ = cJSON_Parse(json);
        if (!root_) { return ESP_ERR_INVALID_ARG; }
        const cJSON* foods = cJSON_GetObjectItemCaseSensitive(root_, "foods");
        const int count = cJSON_GetArraySize(foods);
        if (!cJSON_IsArray(foods) || count < 1 || count > 32) { return ESP_ERR_INVALID_SIZE; }
        for (int i = 0; i < count; ++i) {
            const cJSON* item = cJSON_GetArrayItem(foods, i);
            const cJSON* id = cJSON_GetObjectItemCaseSensitive(item, "id");
            const cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "name");
            if (!cJSON_IsString(id) || !cJSON_IsString(name) || !id->valuestring[0] ||
                !name->valuestring[0] || std::strlen(id->valuestring) >= 32 || std::strlen(name->valuestring) >= 48) {
                return ESP_ERR_INVALID_ARG;
            }
            for (int j = 0; j < i; ++j) {
                if (std::strcmp(all_[j].id, id->valuestring) == 0) { return ESP_ERR_INVALID_ARG; }
            }
            const cJSON* aliases = cJSON_GetObjectItemCaseSensitive(item, "aliases");
            const int alias_count = cJSON_GetArraySize(aliases);
            if ((aliases && !cJSON_IsArray(aliases)) || alias_count > 8) { return ESP_ERR_INVALID_SIZE; }
            for (int j = 0; j < alias_count; ++j) {
                const cJSON* alias = cJSON_GetArrayItem(aliases, j);
                if (!cJSON_IsString(alias)) { return ESP_ERR_INVALID_ARG; }
                aliases_[i][j] = alias->valuestring;
            }
            all_[i] = {id->valuestring, name->valuestring, aliases_[i], static_cast<std::size_t>(alias_count)};
        }
        all_count_ = static_cast<std::size_t>(count);
        return ESP_OK;
    }

    esp_err_t select_model_labels(std::string_view csv, std::size_t background) {
        if (!root_ || !all_count_) { return ESP_ERR_INVALID_STATE; }
        food_count_ = label_count_ = 0;
        while (!csv.empty() && (csv.front() == ' ' || csv.front() == '\t')) { csv.remove_prefix(1); }
        while (!csv.empty() && (csv.back() == ' ' || csv.back() == '\t')) { csv.remove_suffix(1); }
        if (csv.empty()) {
            for (std::size_t i = 0; i < all_count_; ++i) { foods_[food_count_++] = all_[i]; }
            return ESP_OK; // Pet/touch/UI can run; vision waits for explicit class metadata.
        }
        while (!csv.empty()) {
            const std::size_t comma = csv.find(',');
            std::string_view id = csv.substr(0, comma);
            while (!id.empty() && (id.front() == ' ' || id.front() == '\t')) { id.remove_prefix(1); }
            while (!id.empty() && (id.back() == ' ' || id.back() == '\t')) { id.remove_suffix(1); }
            if (id.empty() || food_count_ >= 32) { return ESP_ERR_INVALID_ARG; }
            const PetFood* match = nullptr;
            for (std::size_t i = 0; i < all_count_; ++i) { if (id == all_[i].id) { match = &all_[i]; break; } }
            if (!match) { return ESP_ERR_INVALID_ARG; }
            for (std::size_t i = 0; i < food_count_; ++i) { if (id == foods_[i].id) { return ESP_ERR_INVALID_ARG; } }
            foods_[food_count_++] = *match;
            if (comma == std::string_view::npos) { break; }
            csv.remove_prefix(comma + 1);
            if (csv.empty()) { return ESP_ERR_INVALID_ARG; }
        }
        if (background > food_count_) { return ESP_ERR_INVALID_ARG; }
        label_count_ = food_count_ + 1;
        for (std::size_t channel = 0, food = 0; channel < label_count_; ++channel) {
            if (channel == background) { labels_[channel] = {"", "Background"}; }
            else { labels_[channel] = {foods_[food].id, foods_[food].name}; ++food; }
        }
        return ESP_OK;
    }
    const PetFood* foods() const { return foods_; }
    std::size_t food_count() const { return food_count_; }
    const VisionLabel* labels() const { return labels_; }
    std::size_t label_count() const { return label_count_; }
private:
    cJSON* root_ = nullptr;
    PetFood all_[32] = {}, foods_[32] = {};
    const char* aliases_[32][8] = {};
    VisionLabel labels_[33] = {};
    std::size_t all_count_ = 0, food_count_ = 0, label_count_ = 0;
};

inline CameraHalConfig camera_config() {
    CameraHalConfig config;
    config.pins = {CONFIG_PELY_CAM_PWDN, CONFIG_PELY_CAM_RESET, CONFIG_PELY_CAM_XCLK,
        CONFIG_PELY_CAM_SDA, CONFIG_PELY_CAM_SCL, CONFIG_PELY_CAM_D0, CONFIG_PELY_CAM_D1,
        CONFIG_PELY_CAM_D2, CONFIG_PELY_CAM_D3, CONFIG_PELY_CAM_D4, CONFIG_PELY_CAM_D5,
        CONFIG_PELY_CAM_D6, CONFIG_PELY_CAM_D7, CONFIG_PELY_CAM_VSYNC, CONFIG_PELY_CAM_HREF, CONFIG_PELY_CAM_PCLK};
#ifdef CONFIG_PELY_CAM_MIRROR
    config.mirror_horizontal = true;
#endif
#ifdef CONFIG_PELY_CAM_FLIP
    config.flip_vertical = true;
#endif
    return config;
}

inline VisionAiConfig vision_config(const FoodCatalog& catalog) {
    VisionAiConfig config;
    config.labels = catalog.labels(); config.label_count = catalog.label_count();
    config.background_channel = CONFIG_PELY_FOMO_BACKGROUND_CHANNEL;
    config.input_width = CONFIG_PELY_FOMO_INPUT_WIDTH; config.input_height = CONFIG_PELY_FOMO_INPUT_HEIGHT;
    config.confidence_threshold = CONFIG_PELY_CONFIDENCE_PERCENT / 100.0F;
    config.event_loop = nullptr; // task_vision_ai publishes timestamped, compact input events itself.
#ifdef CONFIG_PELY_FOMO_STRETCH
    config.resize_mode = FomoResizeMode::STRETCH;
#endif
#ifdef CONFIG_PELY_FOMO_LOGITS
    config.output_mode = FomoOutputMode::SOFTMAX_LOGITS;
#endif
#ifdef CONFIG_PELY_FOMO_BGR
    config.bgr_input = true;
#endif
    for (unsigned channel = 0; channel < 3; ++channel) {
#ifdef CONFIG_PELY_FOMO_NORMALIZE_MINUS_ONE
        config.input_scale[channel] = 1.0F / 127.5F; config.input_offset[channel] = -1.0F;
#elif defined(CONFIG_PELY_FOMO_NORMALIZE_RAW)
        config.input_scale[channel] = 1.0F; config.input_offset[channel] = 0.0F;
#else
        config.input_scale[channel] = 1.0F / 255.0F; config.input_offset[channel] = 0.0F;
#endif
    }
    return config;
}

inline LedMatrixConfig matrix_config() {
    LedMatrixConfig config;
    config.gpio_num = CONFIG_PELY_LED_GPIO;
    config.width = CONFIG_PELY_LED_WIDTH; config.height = CONFIG_PELY_LED_HEIGHT;
    config.tile_width = CONFIG_PELY_LED_TILE_WIDTH; config.tile_height = CONFIG_PELY_LED_TILE_HEIGHT;
    config.brightness = CONFIG_PELY_LED_BRIGHTNESS;
#ifdef CONFIG_PELY_LED_COLUMNS
    config.major = MatrixMajor::COLUMNS;
#endif
#ifndef CONFIG_PELY_LED_SERPENTINE
    config.serpentine = false;
#endif
#ifdef CONFIG_PELY_LED_SERPENTINE_TILES
    config.serpentine_tiles = true;
#endif
#ifdef CONFIG_PELY_LED_MIRROR_X
    config.mirror_x = true;
#endif
#ifdef CONFIG_PELY_LED_MIRROR_Y
    config.mirror_y = true;
#endif
    return config;
}

inline TouchSensorConfig touch_config(esp_event_loop_handle_t loop) {
    TouchSensorConfig config;
    config.event_loop = loop;
#ifdef CONFIG_PELY_TOUCH_GPIO
    config.gpio_num = CONFIG_PELY_TOUCH_GPIO;
#endif
    config.pad_channel = CONFIG_PELY_TOUCH_CHANNEL;
#ifdef CONFIG_PELY_TOUCH_INTERNAL
    config.backend = TouchBackend::INTERNAL_PAD;
#endif
#ifndef CONFIG_PELY_TOUCH_ACTIVE_HIGH
    config.active_high = false;
#endif
    return config;
}
} // namespace pely
