#include "led_matrix.hpp"
#include "eye_animation.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "led_strip_rmt.h"

namespace {
class Lock final {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
private:
    SemaphoreHandle_t mutex_;
};

bool valid_config(const LedMatrixConfig& config) {
    if (!GPIO_IS_VALID_OUTPUT_GPIO(config.gpio_num) || config.width < 8 || config.width > 64 ||
        config.height < 4 || config.height > 32 || config.width * config.height > 1024 ||
        (config.major != MatrixMajor::ROWS && config.major != MatrixMajor::COLUMNS) ||
        ((config.tile_width == 0) != (config.tile_height == 0)) || config.transition_ms > 5000 ||
        config.blink_min_ms < 100 || config.blink_max_ms < config.blink_min_ms || config.blink_max_ms > 60000 ||
        config.blink_duration_ms < 40 || config.blink_duration_ms > 1000 ||
        !std::isfinite(config.happy_bounce_pixels) || config.happy_bounce_pixels < 0.0F ||
        config.happy_bounce_pixels > config.height / 4.0F) { return false; }
    return !config.tile_width ||
        (config.width % config.tile_width == 0 && config.height % config.tile_height == 0);
}

uint8_t blend(uint8_t from, uint8_t to, float amount) {
    return static_cast<uint8_t>(std::lround(eye_animation::lerp(from, to, amount)));
}
} // namespace

LedMatrix::LedMatrix() : mutex_(xSemaphoreCreateMutex()) {}
LedMatrix::~LedMatrix() {
    if (strip_) {
        configASSERT(xPortGetCoreID() == 0);
        const esp_err_t cleared = led_strip_clear(strip_);
        const esp_err_t deleted = led_strip_del(strip_);
        configASSERT(cleared == ESP_OK && deleted == ESP_OK);
        (void)cleared; (void)deleted;
    }
    heap_caps_free(pixels_);
    heap_caps_free(transition_from_);
    if (mutex_) { vSemaphoreDelete(mutex_); }
}

esp_err_t LedMatrix::check_context() const {
    if (!mutex_) { return ESP_ERR_NO_MEM; }
    return xPortGetCoreID() == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t LedMatrix::init(const LedMatrixConfig& config) {
    const esp_err_t context = check_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (strip_) { return ESP_ERR_INVALID_STATE; }
    if (!valid_config(config)) { return ESP_ERR_INVALID_ARG; }
    config_ = config;
    const std::size_t count = config.width * config.height;
    pixels_ = static_cast<EyeRgb*>(heap_caps_calloc(count, sizeof(EyeRgb), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    transition_from_ = static_cast<EyeRgb*>(heap_caps_calloc(count, sizeof(EyeRgb), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!pixels_ || !transition_from_) {
        heap_caps_free(pixels_); pixels_ = nullptr;
        heap_caps_free(transition_from_); transition_from_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    led_strip_config_t strip_config{};
    strip_config.strip_gpio_num = config.gpio_num;
    strip_config.max_leds = static_cast<uint32_t>(count);
    strip_config.led_model = LED_MODEL_WS2812;
    // Assign fields instead of the library's C compound-literal macro (C++17).
    strip_config.color_component_format.format.r_pos = 1;
    strip_config.color_component_format.format.g_pos = 0;
    strip_config.color_component_format.format.b_pos = 2;
    strip_config.color_component_format.format.w_pos = 3;
    strip_config.color_component_format.format.bytes_per_color = 1;
    strip_config.color_component_format.format.num_components = 3;
    led_strip_rmt_config_t rmt{};
    rmt.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt.resolution_hz = 10000000;
    rmt.mem_block_symbols = config.rmt_dma ? 256 : 64;
    rmt.flags.with_dma = config.rmt_dma;
    const esp_err_t error = led_strip_new_rmt_device(&strip_config, &rmt, &strip_);
    if (error != ESP_OK) {
        heap_caps_free(pixels_); pixels_ = nullptr;
        heap_caps_free(transition_from_); transition_from_ = nullptr;
        return error;
    }
    emotion_ = Emotion::IDLE;
    emotion_since_us_ = transition_since_us_ = esp_timer_get_time();
    next_blink_us_ = emotion_since_us_ + next_blink_delay();
    blink_since_us_ = -1;
    transitioning_ = paused_ = false;
    // If this refresh fails, retain the handle so callers can retry/update/deinit.
    return render_locked(emotion_since_us_, true);
}

int64_t LedMatrix::next_blink_delay() const {
    return (config_.blink_min_ms + esp_random() % (config_.blink_max_ms - config_.blink_min_ms + 1)) * int64_t{1000};
}

uint32_t LedMatrix::pixel_index(uint16_t x, uint16_t y) const {
    if (config_.mirror_x) { x = config_.width - 1 - x; }
    if (config_.mirror_y) { y = config_.height - 1 - y; }
    const uint32_t tile_width = config_.tile_width ? config_.tile_width : config_.width;
    const uint32_t tile_height = config_.tile_height ? config_.tile_height : config_.height;
    const uint32_t tile_columns = config_.width / tile_width;
    const uint32_t tile_y = y / tile_height;
    uint32_t tile_x = x / tile_width;
    if (config_.serpentine_tiles && (tile_y & 1)) { tile_x = tile_columns - 1 - tile_x; }
    const uint32_t tile_offset = (tile_y * tile_columns + tile_x) * tile_width * tile_height;
    uint32_t local_x = x % tile_width, local_y = y % tile_height;
    if (config_.major == MatrixMajor::ROWS) {
        if (config_.serpentine && (local_y & 1)) { local_x = tile_width - 1 - local_x; }
        return tile_offset + local_y * tile_width + local_x;
    }
    if (config_.serpentine && (local_x & 1)) { local_y = tile_height - 1 - local_y; }
    return tile_offset + local_x * tile_height + local_y;
}

EyeRgb LedMatrix::expression_pixel(Emotion state, float x, float y, float openness, float bounce) const {
    const bool left = x < config_.width / 2.0F;
    const float cx = config_.width * (left ? 0.25F : 0.75F);
    const float cy = config_.height * 0.5F + bounce;
    const float rx = config_.width * 0.16F;
    const float u = (x - cx) / rx;
    float ry = config_.height * 0.28F;
    EyeRgb color = config_.idle_color;
    switch (state) {
    case Emotion::HAPPY: color = config_.happy_color; ry *= 0.85F; break;
    case Emotion::SAD: color = config_.sad_color; ry *= 0.75F; break;
    case Emotion::ANGRY: color = config_.angry_color; break;
    case Emotion::LONELY: color = config_.sad_color; ry *= 0.80F; break;
    case Emotion::SLEEPY: ry *= 0.45F; break;
    case Emotion::SURPRISED: ry *= 1.15F; break;
    case Emotion::CONFUSED: ry *= left ? 0.9F : 0.6F; break;
    case Emotion::HUNGRY: ry *= 0.82F; break;
    case Emotion::DEAD:
        if (std::fabs(u) <= 1.0F && std::fabs(y - cy) <= ry &&
            std::fabs(std::fabs(u) - std::fabs((y - cy) / ry)) < 0.30F) { return config_.sad_color; }
        return {};
    case Emotion::IDLE: break;
    }
    if (std::fabs(u) > 1.0F) { return {}; }
    if (openness < 0.18F) {
        return static_cast<int>(y) == static_cast<int>(cy) ? color : EyeRgb{};
    }
    const float v = (y - cy) / (ry * openness);
    if (state == Emotion::HAPPY) {
        return std::fabs(v - (-0.6F + 1.2F * u * u)) <= 0.30F ? color : EyeRgb{};
    }
    if (u * u + v * v > 1.0F) { return {}; }
    const float inward_u = left ? u : -u;
    if ((state == Emotion::SAD || state == Emotion::LONELY) && v < -0.25F - 0.5F * inward_u) { return {}; }
    if (state == Emotion::ANGRY && v < -0.2F + 0.7F * inward_u) { return {}; }
    return color;
}

esp_err_t LedMatrix::flush_locked() {
    for (uint16_t y = 0; y < config_.height; ++y) {
        for (uint16_t x = 0; x < config_.width; ++x) {
            const EyeRgb color = pixels_[y * config_.width + x];
            const auto scale = [this](uint8_t component) {
                return (static_cast<uint32_t>(component) * config_.brightness + 127) / 255;
            };
            const esp_err_t error = led_strip_set_pixel(strip_, pixel_index(x, y),
                scale(color.red), scale(color.green), scale(color.blue));
            if (error != ESP_OK) { return error; }
        }
    }
    return led_strip_refresh(strip_);
}

esp_err_t LedMatrix::render_locked(int64_t now, bool transmit) {
    if (paused_) { return ESP_OK; }
    float openness = 1.0F;
    if (config_.auto_blink && emotion_ != Emotion::DEAD) {
        if (blink_since_us_ < 0 && now >= next_blink_us_) { blink_since_us_ = now; }
        const float duration = config_.blink_duration_ms * (emotion_ == Emotion::SLEEPY ? 2.2F : 1.0F) / 1000.0F;
        if (blink_since_us_ >= 0) {
            const float elapsed = static_cast<float>(now - blink_since_us_) / 1000000.0F;
            if (elapsed >= duration) { blink_since_us_ = -1; next_blink_us_ = now + next_blink_delay(); }
            else { openness = eye_animation::calculate_blink_progress(elapsed, duration); }
        }
    }
    float progress = 1.0F;
    if (transitioning_) {
        progress = config_.transition_ms ? eye_animation::clamp01(
            static_cast<float>(now - transition_since_us_) / (config_.transition_ms * 1000.0F)) : 1.0F;
        if (progress >= 1.0F) { transitioning_ = false; }
    }
    const float fade = eye_animation::ease_in_out(progress);
    const float bounce = emotion_ == Emotion::HAPPY ? config_.happy_bounce_pixels *
        std::sin(static_cast<float>(now - emotion_since_us_) / 1000000.0F * 4.0F * 2.0F * eye_animation::kPi) : 0.0F;
    for (uint16_t y = 0; y < config_.height; ++y) {
        for (uint16_t x = 0; x < config_.width; ++x) {
            const std::size_t index = y * config_.width + x;
            const EyeRgb target = expression_pixel(emotion_, x + 0.5F, y + 0.5F, openness, bounce);
            const EyeRgb from = transition_from_[index];
            pixels_[index] = {blend(from.red, target.red, fade), blend(from.green, target.green, fade),
                blend(from.blue, target.blue, fade)};
        }
    }
    return transmit ? flush_locked() : ESP_OK;
}

esp_err_t LedMatrix::show_emotion(Emotion state) {
    if (static_cast<unsigned>(state) > static_cast<unsigned>(Emotion::DEAD)) { return ESP_ERR_INVALID_ARG; }
    const esp_err_t context = check_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!strip_) { return ESP_ERR_INVALID_STATE; }
    const int64_t now = esp_timer_get_time();
    if (state != emotion_ || paused_) {
        render_locked(now, false);
        std::memcpy(transition_from_, pixels_, config_.width * config_.height * sizeof(EyeRgb));
        emotion_ = state;
        emotion_since_us_ = transition_since_us_ = now;
        transitioning_ = config_.transition_ms != 0 && state != Emotion::DEAD;
        blink_since_us_ = -1;
        next_blink_us_ = now + next_blink_delay();
        paused_ = false;
    }
    return render_locked(now, true);
}

esp_err_t LedMatrix::update_tick() {
    const esp_err_t context = check_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!strip_) { return ESP_ERR_INVALID_STATE; }
    return render_locked(esp_timer_get_time(), true);
}

esp_err_t LedMatrix::set_brightness(uint8_t brightness) {
    const esp_err_t context = check_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!strip_) { return ESP_ERR_INVALID_STATE; }
    config_.brightness = brightness;
    return flush_locked();
}

esp_err_t LedMatrix::clear() {
    const esp_err_t context = check_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!strip_) { return ESP_ERR_INVALID_STATE; }
    const esp_err_t error = led_strip_clear(strip_);
    if (error == ESP_OK) {
        std::fill_n(pixels_, config_.width * config_.height, EyeRgb{});
        paused_ = true;
    }
    return error;
}

esp_err_t LedMatrix::deinit() {
    const esp_err_t context = check_context();
    if (context != ESP_OK) { return context; }
    Lock lock(mutex_);
    if (!strip_) { return ESP_ERR_INVALID_STATE; }
    esp_err_t error = led_strip_clear(strip_);
    if (error == ESP_OK) { error = led_strip_del(strip_); }
    if (error != ESP_OK) { return error; }
    strip_ = nullptr;
    heap_caps_free(pixels_); pixels_ = nullptr;
    heap_caps_free(transition_from_); transition_from_ = nullptr;
    return ESP_OK;
}
