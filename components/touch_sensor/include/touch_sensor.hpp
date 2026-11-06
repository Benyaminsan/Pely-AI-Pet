#pragma once

#include <cstdint>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

ESP_EVENT_DECLARE_BASE(TOUCH_SENSOR_EVENT);
enum TouchSensorEventId : int32_t { TOUCH_EVENT_TOUCHED = 1 };
enum class TouchBackend : uint8_t { TTP223_GPIO, INTERNAL_PAD };

struct TouchSensorConfig {
    TouchBackend backend = TouchBackend::TTP223_GPIO;
    int gpio_num = -1;            // TTP223 OUT, momentary mode; never its power pin.
    bool active_high = true;
    uint8_t pad_channel = 0;       // ESP32-S3 touch channel 1..14; 0 is reserved.
    float threshold_ratio = 0.20F; // Delta above benchmark, not an absolute threshold.
    uint32_t calibration_ms = 100;
    uint32_t debounce_ms = 400;    // Matches the Python sensor's accepted-touch spacing.
    uint8_t queue_length = 16;
    esp_event_loop_handle_t event_loop = nullptr; // Borrowed Core-0 loop.
};

struct TouchSensorEvent {
    TouchBackend backend = TouchBackend::TTP223_GPIO;
    int gpio_num = -1;
    uint8_t pad_channel = 0;
    int64_t timestamp_us = 0;      // Captured in the interrupt, not at dispatch time.
    uint32_t sequence = 0;
};

/** One Core-0 owner task; this object must live in internal RAM (ISR-accessed). */
class TouchSensor final {
public:
    TouchSensor() = default;
    ~TouchSensor();
    TouchSensor(const TouchSensor&) = delete;
    TouchSensor& operator=(const TouchSensor&) = delete;
    TouchSensor(TouchSensor&&) = delete;
    TouchSensor& operator=(TouchSensor&&) = delete;

    esp_err_t init(const TouchSensorConfig& config);
    // Blocks on the ISR queue while idle; no runtime GPIO/capacitance polling.
    // ESP_OK means one accepted touch. Posting failures are separate from detection.
    // A queued stop returns ESP_ERR_INVALID_STATE; a hardware timeout is resumed
    // and reported as ESP_ERR_INVALID_RESPONSE. No touch event is emitted for either.
    esp_err_t wait_for_touch(TouchSensorEvent& result, TickType_t wait = portMAX_DELAY,
                             esp_err_t* event_error = nullptr);
    // May be called by another task while the owner waits. Join the owner before destruction.
    esp_err_t request_stop(TickType_t wait = 0);
    esp_err_t deinit();
    uint32_t dropped_interrupts() const;
    uint32_t calibrated_threshold() const { return calibrated_threshold_; }
private:
    enum class RawKind : uint8_t { TOUCH, HARDWARE_TIMEOUT, STOP };
    struct RawEvent { int64_t timestamp_us; RawKind kind; };
    static void IRAM_ATTR gpio_isr(void* argument);
    static void IRAM_ATTR pad_isr(void* argument);
    void IRAM_ATTR enqueue_isr(RawKind kind);
    bool on_owner_task() const;
    esp_err_t cleanup();
    esp_err_t init_gpio();
    esp_err_t init_pad();

    TouchSensorConfig config_;
    TaskHandle_t owner_task_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    mutable portMUX_TYPE stats_lock_ = portMUX_INITIALIZER_UNLOCKED;
    uint32_t dropped_ = 0;
    bool timeout_resume_pending_ = false;
    uint32_t sequence_ = 0;
    uint32_t calibrated_threshold_ = 0;
    int64_t last_touch_us_ = -1;
    bool initialized_ = false;
    bool gpio_configured_ = false;
    bool gpio_handler_added_ = false;
    bool pad_initialized_ = false;
    bool pad_handler_added_ = false;
    bool fsm_started_ = false;
};
