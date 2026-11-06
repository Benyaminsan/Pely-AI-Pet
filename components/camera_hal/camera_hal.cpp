#include "camera_hal.hpp"

#include <utility>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

namespace {
bool valid_pins(const Ov5640Pins& pins) {
    const int all[] = {pins.pwdn, pins.reset, pins.xclk, pins.sda, pins.scl,
        pins.d0, pins.d1, pins.d2, pins.d3, pins.d4, pins.d5, pins.d6, pins.d7,
        pins.vsync, pins.href, pins.pclk};
    for (std::size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        if (i < 2 && all[i] == -1) { continue; }
        if (!GPIO_IS_VALID_GPIO(all[i])) { return false; }
        if (i < 5 && !GPIO_IS_VALID_OUTPUT_GPIO(all[i])) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (all[i] == all[j]) { return false; }
        }
    }
    return true;
}
} // namespace

CameraHal::Frame::~Frame() { reset(); }
CameraHal::Frame::Frame(Frame&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), buffer_(std::exchange(other.buffer_, nullptr)) {}
CameraHal::Frame& CameraHal::Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        reset();
        owner_ = std::exchange(other.owner_, nullptr);
        buffer_ = std::exchange(other.buffer_, nullptr);
    }
    return *this;
}
void CameraHal::Frame::reset() { if (owner_) { owner_->return_frame(*this); } }
Rgb565FrameView CameraHal::Frame::view() const {
    if (!buffer_) { return {}; }
    return {buffer_->buf, buffer_->len, static_cast<uint16_t>(buffer_->width),
        static_cast<uint16_t>(buffer_->height), buffer_->width * 2,
        Rgb565ByteOrder::HIGH_BYTE_FIRST};
}

bool CameraHal::on_owner_task() const {
    return xPortGetCoreID() == 1 && owner_task_ == xTaskGetCurrentTaskHandle();
}
CameraHal::~CameraHal() {
    if (initialized_) {
        // Destroy on the same worker after every Frame lease has been released.
        configASSERT(on_owner_task() && !outstanding_);
        esp_camera_deinit();
    }
}

esp_err_t CameraHal::init(const CameraHalConfig& config) {
    if (xPortGetCoreID() != 1 || initialized_) { return ESP_ERR_INVALID_STATE; }
#if !defined(CONFIG_CAMERA_CORE1) || !CONFIG_CAMERA_CORE1
    // esp32-camera creates an internal task; the caller's affinity alone is insufficient.
    return ESP_ERR_INVALID_STATE;
#endif
    if (!valid_pins(config.pins) || config.xclk_hz < 10000000 || config.xclk_hz > 24000000 ||
        config.frame_size < FRAMESIZE_96X96 || config.frame_size > FRAMESIZE_QVGA) {
        return ESP_ERR_INVALID_ARG;
    }
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) { return ESP_ERR_NO_MEM; }
    camera_config_t driver{};
    driver.pin_pwdn = config.pins.pwdn;
    driver.pin_reset = config.pins.reset;
    driver.pin_xclk = config.pins.xclk;
    driver.pin_sccb_sda = config.pins.sda;
    driver.pin_sccb_scl = config.pins.scl;
    driver.pin_d0 = config.pins.d0; driver.pin_d1 = config.pins.d1;
    driver.pin_d2 = config.pins.d2; driver.pin_d3 = config.pins.d3;
    driver.pin_d4 = config.pins.d4; driver.pin_d5 = config.pins.d5;
    driver.pin_d6 = config.pins.d6; driver.pin_d7 = config.pins.d7;
    driver.pin_vsync = config.pins.vsync;
    driver.pin_href = config.pins.href;
    driver.pin_pclk = config.pins.pclk;
    driver.xclk_freq_hz = config.xclk_hz;
    driver.ledc_timer = config.ledc_timer;
    driver.ledc_channel = config.ledc_channel;
    driver.pixel_format = PIXFORMAT_RGB565;
    driver.frame_size = config.frame_size;
    driver.fb_count = 1; // The driver's continuous multi-buffer path is intended for JPEG.
    driver.fb_location = CAMERA_FB_IN_PSRAM;
    driver.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    esp_err_t error = esp_camera_init(&driver);
    if (error != ESP_OK) { return error; }
    sensor_t* sensor = esp_camera_sensor_get();
    if (!sensor || sensor->id.PID != OV5640_PID) {
        esp_camera_deinit();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!sensor->set_hmirror || !sensor->set_vflip ||
        sensor->set_hmirror(sensor, config.mirror_horizontal ? 1 : 0) != 0 ||
        sensor->set_vflip(sensor, config.flip_vertical ? 1 : 0) != 0) {
        esp_camera_deinit();
        return ESP_FAIL;
    }
    owner_task_ = xTaskGetCurrentTaskHandle();
    initialized_ = true;
    return ESP_OK;
}

esp_err_t CameraHal::capture(Frame& result) {
    if (!initialized_ || !on_owner_task() || outstanding_ || result) { return ESP_ERR_INVALID_STATE; }
    camera_fb_t* frame = esp_camera_fb_get();
    if (!frame) { return ESP_ERR_TIMEOUT; }
    if (frame->format != PIXFORMAT_RGB565 || !frame->buf || !frame->width || !frame->height ||
        frame->width > 4096 || frame->height > 4096 || frame->len < frame->width * frame->height * 2) {
        esp_camera_fb_return(frame);
        return ESP_ERR_INVALID_SIZE;
    }
    outstanding_ = frame;
    result.buffer_ = frame;
    result.owner_ = this;
    return ESP_OK;
}

void CameraHal::return_frame(Frame& frame) {
    configASSERT(on_owner_task() && frame.buffer_ == outstanding_);
    esp_camera_fb_return(frame.buffer_);
    outstanding_ = nullptr;
    frame.buffer_ = nullptr;
    frame.owner_ = nullptr;
}

esp_err_t CameraHal::deinit() {
    if (!initialized_ || !on_owner_task() || outstanding_) { return ESP_ERR_INVALID_STATE; }
    const esp_err_t error = esp_camera_deinit();
    if (error == ESP_OK) { initialized_ = false; owner_task_ = nullptr; }
    return error;
}
