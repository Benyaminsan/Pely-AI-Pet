#include "touch_sensor.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

#include "driver/gpio.h"
#include "driver/touch_pad.h"
#include "esp_intr_alloc.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"

ESP_EVENT_DEFINE_BASE(TOUCH_SENSOR_EVENT);
static_assert(std::is_trivially_copyable<TouchSensorEvent>::value, "Events must be copyable");

namespace {
constexpr auto kPadInterrupts = static_cast<touch_pad_intr_mask_t>(
    TOUCH_PAD_INTR_MASK_ACTIVE | TOUCH_PAD_INTR_MASK_TIMEOUT);
}

bool TouchSensor::on_owner_task() const {
    return xPortGetCoreID() == 0 && owner_task_ == xTaskGetCurrentTaskHandle();
}

TouchSensor::~TouchSensor() {
    if (queue_) {
        configASSERT(on_owner_task());
        const esp_err_t error = cleanup();
        configASSERT(error == ESP_OK); // Never destroy storage while an ISR still refers to it.
        (void)error;
    }
}

esp_err_t TouchSensor::init(const TouchSensorConfig& config) {
    if (queue_ || xPortGetCoreID() != 0) { return ESP_ERR_INVALID_STATE; }
    if (!esp_ptr_internal(this) || !config.event_loop || config.queue_length == 0 || config.queue_length > 64 ||
        config.debounce_ms > 60000 ||
        (config.backend != TouchBackend::TTP223_GPIO && config.backend != TouchBackend::INTERNAL_PAD)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config.backend == TouchBackend::TTP223_GPIO) {
        if (!GPIO_IS_VALID_GPIO(config.gpio_num)) { return ESP_ERR_INVALID_ARG; }
    } else if (config.pad_channel < 1 || config.pad_channel > 14 ||
               !std::isfinite(config.threshold_ratio) || config.threshold_ratio <= 0.0F ||
               config.threshold_ratio >= 1.0F || config.calibration_ms < 50 || config.calibration_ms > 2000) {
        return ESP_ERR_INVALID_ARG;
    }
    config_ = config;
    owner_task_ = xTaskGetCurrentTaskHandle();
    queue_ = xQueueCreate(config.queue_length, sizeof(RawEvent));
    if (!queue_) { owner_task_ = nullptr; return ESP_ERR_NO_MEM; }
    last_touch_us_ = -1;
    sequence_ = dropped_ = calibrated_threshold_ = 0;
    timeout_resume_pending_ = false;
    const esp_err_t error = config.backend == TouchBackend::TTP223_GPIO ? init_gpio() : init_pad();
    if (error != ESP_OK) {
        // A cleanup error retains ISR-visible storage; deinit can then be retried.
        const esp_err_t cleaned = cleanup();
        return cleaned != ESP_OK ? cleaned : error;
    }
    initialized_ = true;
    return ESP_OK;
}

esp_err_t TouchSensor::init_gpio() {
    gpio_config_t gpio{};
    gpio.pin_bit_mask = uint64_t{1} << config_.gpio_num;
    gpio.mode = GPIO_MODE_INPUT;
    gpio.intr_type = GPIO_INTR_DISABLE;
    esp_err_t error = gpio_config(&gpio);
    if (error != ESP_OK) { return error; }
    gpio_configured_ = true;
    error = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) { return error; }
    // Reuse the shared service; never uninstall it while other GPIO clients may use it.
    error = gpio_isr_handler_add(static_cast<gpio_num_t>(config_.gpio_num), gpio_isr, this);
    if (error != ESP_OK) { return error; }
    gpio_handler_added_ = true;
    error = gpio_set_intr_type(static_cast<gpio_num_t>(config_.gpio_num),
        config_.active_high ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE);
    if (error == ESP_OK) { error = gpio_intr_enable(static_cast<gpio_num_t>(config_.gpio_num)); }
    return error;
}

esp_err_t TouchSensor::init_pad() {
    esp_err_t error = touch_pad_init();
    if (error != ESP_OK) { return error; }
    pad_initialized_ = true;
    const auto pad = static_cast<touch_pad_t>(config_.pad_channel);
    error = touch_pad_config(pad);
    if (error == ESP_OK) { error = touch_pad_set_thresh(pad, TOUCH_PAD_THRESHOLD_MAX); }
    touch_filter_config_t filter{};
    filter.mode = TOUCH_PAD_FILTER_IIR_16;
    filter.debounce_cnt = 1;
    filter.noise_thr = 0;
    filter.jitter_step = 4;
    filter.smh_lvl = TOUCH_PAD_SMOOTH_IIR_2;
    if (error == ESP_OK) { error = touch_pad_filter_set_config(&filter); }
    if (error == ESP_OK) { error = touch_pad_filter_enable(); }
    if (error == ESP_OK) { error = touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER); }
    if (error == ESP_OK) { error = touch_pad_fsm_start(); }
    if (error != ESP_OK) { return error; }
    fsm_started_ = true;
    // One initialization-only settling wait and benchmark read, not a polling loop.
    vTaskDelay(std::max<TickType_t>(1, pdMS_TO_TICKS(config_.calibration_ms)));
    uint32_t benchmark = 0;
    error = touch_pad_read_benchmark(pad, &benchmark);
    if (error != ESP_OK) { return error; }
    if (benchmark == 0 || benchmark >= TOUCH_PAD_THRESHOLD_MAX) { return ESP_ERR_INVALID_RESPONSE; }
    calibrated_threshold_ = std::max<uint32_t>(1,
        static_cast<uint32_t>(benchmark * static_cast<double>(config_.threshold_ratio)));
    error = touch_pad_set_thresh(pad, calibrated_threshold_);
    if (error == ESP_OK) { error = touch_pad_timeout_set(true, TOUCH_PAD_THRESHOLD_MAX); }
    if (error == ESP_OK) { error = touch_pad_isr_register(pad_isr, this, kPadInterrupts); }
    if (error != ESP_OK) { return error; }
    pad_handler_added_ = true;
    error = touch_pad_intr_clear(kPadInterrupts);
    if (error == ESP_OK) { error = touch_pad_intr_enable(kPadInterrupts); }
    return error;
}

// IRAM_ATTR is on the declarations; repeating its unique section attribute here
// would conflict with the section assigned by ESP-IDF's __COUNTER__ macro.
void TouchSensor::enqueue_isr(RawKind kind) {
    const RawEvent event{esp_timer_get_time(), kind};
    BaseType_t awakened = pdFALSE;
    if (xQueueSendFromISR(queue_, &event, &awakened) != pdTRUE) {
        portENTER_CRITICAL_ISR(&stats_lock_);
        if (dropped_ != UINT32_MAX) { ++dropped_; }
        if (kind == RawKind::HARDWARE_TIMEOUT) { timeout_resume_pending_ = true; }
        portEXIT_CRITICAL_ISR(&stats_lock_);
    }
    if (awakened == pdTRUE) { portYIELD_FROM_ISR(); }
}

void TouchSensor::gpio_isr(void* argument) {
    static_cast<TouchSensor*>(argument)->enqueue_isr(RawKind::TOUCH);
}

void TouchSensor::pad_isr(void* argument) {
    auto* sensor = static_cast<TouchSensor*>(argument);
    const uint32_t status = touch_pad_read_intr_status_mask();
    if (status & TOUCH_PAD_INTR_MASK_TIMEOUT) {
        sensor->enqueue_isr(RawKind::HARDWARE_TIMEOUT);
    } else if ((status & TOUCH_PAD_INTR_MASK_ACTIVE) &&
               touch_pad_get_current_meas_channel() == sensor->config_.pad_channel) {
        sensor->enqueue_isr(RawKind::TOUCH);
    }
}

esp_err_t TouchSensor::wait_for_touch(TouchSensorEvent& result, TickType_t wait, esp_err_t* event_error) {
    result = TouchSensorEvent{};
    if (event_error) { *event_error = ESP_OK; }
    if (!initialized_ || !on_owner_task()) { return ESP_ERR_INVALID_STATE; }
    const TickType_t started = xTaskGetTickCount();
    TickType_t remaining = wait;
    RawEvent raw{};
    for (;;) {
        // A full queue must not permanently stall the hardware FSM by losing
        // its timeout notification. A full queue already guarantees a wakeup.
        portENTER_CRITICAL(&stats_lock_);
        const bool resume = timeout_resume_pending_;
        timeout_resume_pending_ = false;
        portEXIT_CRITICAL(&stats_lock_);
        if (resume) {
            const esp_err_t resumed = touch_pad_timeout_resume();
            return resumed == ESP_OK ? ESP_ERR_INVALID_RESPONSE : resumed;
        }
        if (xQueueReceive(queue_, &raw, remaining) != pdTRUE) { return ESP_ERR_TIMEOUT; }
        if (raw.kind == RawKind::STOP) { return ESP_ERR_INVALID_STATE; }
        if (raw.kind == RawKind::HARDWARE_TIMEOUT) {
            const esp_err_t resumed = touch_pad_timeout_resume();
            return resumed == ESP_OK ? ESP_ERR_INVALID_RESPONSE : resumed;
        }
        if (last_touch_us_ < 0 || raw.timestamp_us - last_touch_us_ >= config_.debounce_ms * int64_t{1000}) {
            last_touch_us_ = raw.timestamp_us;
            if (sequence_ != UINT32_MAX) { ++sequence_; }
            result = {config_.backend, config_.gpio_num, config_.pad_channel, raw.timestamp_us, sequence_};
            const esp_err_t posted = esp_event_post_to(config_.event_loop, TOUCH_SENSOR_EVENT,
                TOUCH_EVENT_TOUCHED, &result, sizeof(result), 0);
            if (event_error) { *event_error = posted; }
            return ESP_OK;
        }
        if (wait != portMAX_DELAY) {
            const TickType_t elapsed = xTaskGetTickCount() - started;
            remaining = elapsed >= wait ? 0 : wait - elapsed;
        }
    }
}

esp_err_t TouchSensor::request_stop(TickType_t wait) {
    if (!initialized_) { return ESP_ERR_INVALID_STATE; }
    const RawEvent stop{0, RawKind::STOP};
    return xQueueSendToFront(queue_, &stop, wait) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

uint32_t TouchSensor::dropped_interrupts() const {
    portENTER_CRITICAL(&stats_lock_);
    const uint32_t dropped = dropped_;
    portEXIT_CRITICAL(&stats_lock_);
    return dropped;
}

esp_err_t TouchSensor::cleanup() {
    if (gpio_handler_added_) {
        esp_err_t error = gpio_intr_disable(static_cast<gpio_num_t>(config_.gpio_num));
        if (error != ESP_OK) { return error; }
        error = gpio_isr_handler_remove(static_cast<gpio_num_t>(config_.gpio_num));
        if (error != ESP_OK) { return error; }
        gpio_handler_added_ = false;
    }
    if (pad_handler_added_) {
        esp_err_t error = touch_pad_intr_disable(kPadInterrupts);
        if (error != ESP_OK) { return error; }
        error = touch_pad_isr_deregister(pad_isr, this);
        if (error != ESP_OK) { return error; }
        pad_handler_added_ = false;
    }
    if (fsm_started_) {
        const esp_err_t error = touch_pad_fsm_stop();
        if (error != ESP_OK) { return error; }
        fsm_started_ = false;
    }
    if (pad_initialized_) {
        const esp_err_t error = touch_pad_deinit();
        if (error != ESP_OK) { return error; }
        pad_initialized_ = false;
    }
    if (gpio_configured_) {
        const esp_err_t error = gpio_reset_pin(static_cast<gpio_num_t>(config_.gpio_num));
        if (error != ESP_OK) { return error; }
        gpio_configured_ = false;
    }
    if (queue_) { vQueueDelete(queue_); queue_ = nullptr; }
    initialized_ = false;
    owner_task_ = nullptr;
    return ESP_OK;
}

esp_err_t TouchSensor::deinit() {
    if (!queue_ || !on_owner_task()) { return ESP_ERR_INVALID_STATE; }
    return cleanup();
}
