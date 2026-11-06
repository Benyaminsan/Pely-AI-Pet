#pragma once

#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"

enum class Emotion : uint8_t {
    IDLE, HAPPY, SAD, ANGRY, HUNGRY, LONELY, SLEEPY, SURPRISED, CONFUSED, DEAD
};
enum class MatrixMajor : uint8_t { ROWS, COLUMNS };
struct EyeRgb { uint8_t red = 0, green = 0, blue = 0; };

struct LedMatrixConfig {
    int gpio_num = -1;
    uint16_t width = 16, height = 8;
    uint16_t tile_width = 0, tile_height = 0; // Both 0: one whole panel.
    MatrixMajor major = MatrixMajor::ROWS;   // Pixel order inside each tile.
    bool serpentine = true;
    bool serpentine_tiles = false;          // Tiles themselves are ordered by rows.
    bool mirror_x = false, mirror_y = false;
    uint8_t brightness = 24;                // Global scale 0..255, applied once.
    bool rmt_dma = true;                    // ESP32-S3 supports RMT DMA.
    uint32_t transition_ms = 280;
    bool auto_blink = true;
    uint32_t blink_min_ms = 2500, blink_max_ms = 6000, blink_duration_ms = 160;
    float happy_bounce_pixels = 0.35F;
    EyeRgb idle_color{64, 192, 255};
    EyeRgb happy_color{32, 255, 96};
    EyeRgb sad_color{32, 96, 255};
    EyeRgb angry_color{255, 40, 16};
};

/** Core-0 UI only. A mutex serializes event handlers and a periodic rendering task. */
class LedMatrix final {
public:
    LedMatrix();
    ~LedMatrix();
    LedMatrix(const LedMatrix&) = delete;
    LedMatrix& operator=(const LedMatrix&) = delete;
    LedMatrix(LedMatrix&&) = delete;
    LedMatrix& operator=(LedMatrix&&) = delete;

    esp_err_t init(const LedMatrixConfig& config);
    // Select expression, start a smooth transition, and refresh one frame.
    // Repeating the same state does not restart animation timers.
    esp_err_t show_emotion(Emotion state);
    esp_err_t update_tick(); // Call from a Core-0 FreeRTOS task at e.g. 30 FPS.
    esp_err_t set_brightness(uint8_t brightness);
    esp_err_t clear();       // Pauses rendering until the next show_emotion().
    esp_err_t deinit();      // Stop callers before destroying the object.
private:
    esp_err_t check_context() const;
    esp_err_t render_locked(int64_t now, bool transmit);
    esp_err_t flush_locked();
    uint32_t pixel_index(uint16_t x, uint16_t y) const;
    int64_t next_blink_delay() const;
    EyeRgb expression_pixel(Emotion state, float x, float y, float openness, float bounce) const;
    LedMatrixConfig config_;
    SemaphoreHandle_t mutex_ = nullptr;
    led_strip_handle_t strip_ = nullptr;
    EyeRgb* pixels_ = nullptr;              // Logical row-major framebuffer.
    EyeRgb* transition_from_ = nullptr;
    Emotion emotion_ = Emotion::IDLE;
    int64_t emotion_since_us_ = 0;
    int64_t transition_since_us_ = 0;
    int64_t blink_since_us_ = -1;
    int64_t next_blink_us_ = 0;
    bool transitioning_ = false;
    bool paused_ = false;
};
