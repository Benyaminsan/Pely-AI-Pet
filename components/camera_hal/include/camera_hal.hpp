#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_camera.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum class Rgb565ByteOrder : uint8_t { HIGH_BYTE_FIRST, LOW_BYTE_FIRST };

// Borrowed pixels. The capture lease must remain alive until inference completes.
struct Rgb565FrameView {
    const uint8_t* data = nullptr;
    std::size_t size_bytes = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    std::size_t stride_bytes = 0;
    Rgb565ByteOrder byte_order = Rgb565ByteOrder::HIGH_BYTE_FIRST;
};

// N16R8 describes the module's memory, not a board's camera wiring.
struct Ov5640Pins {
    int pwdn = -1;
    int reset = -1;
    int xclk = -1;
    int sda = -1;
    int scl = -1;
    int d0 = -1;
    int d1 = -1;
    int d2 = -1;
    int d3 = -1;
    int d4 = -1;
    int d5 = -1;
    int d6 = -1;
    int d7 = -1;
    int vsync = -1;
    int href = -1;
    int pclk = -1;
};

struct CameraHalConfig {
    Ov5640Pins pins;
    framesize_t frame_size = FRAMESIZE_QVGA; // 320x240 RGB565; one PSRAM buffer.
    int xclk_hz = 20000000;
    ledc_timer_t ledc_timer = LEDC_TIMER_0;
    ledc_channel_t ledc_channel = LEDC_CHANNEL_0;
    bool mirror_horizontal = false;
    bool flip_vertical = false;
};

/** Owns the global esp32-camera driver. Use one instance from one Core-1 task. */
class CameraHal final {
public:
    class Frame final {
    public:
        Frame() = default;
        ~Frame();
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
        Frame(Frame&& other) noexcept;
        Frame& operator=(Frame&& other) noexcept;
        explicit operator bool() const { return buffer_ != nullptr; }
        Rgb565FrameView view() const;
        void reset(); // Return the buffer on the owning Core-1 task.
    private:
        friend class CameraHal;
        CameraHal* owner_ = nullptr;
        camera_fb_t* buffer_ = nullptr;
    };

    CameraHal() = default;
    ~CameraHal();
    CameraHal(const CameraHal&) = delete;
    CameraHal& operator=(const CameraHal&) = delete;
    CameraHal(CameraHal&&) = delete;
    CameraHal& operator=(CameraHal&&) = delete;

    esp_err_t init(const CameraHalConfig& config);
    esp_err_t capture(Frame& result); // May block in esp_camera_fb_get; never call from UI.
    esp_err_t deinit();              // Fails while a Frame lease is outstanding.
private:
    friend class Frame;
    void return_frame(Frame& frame);
    bool on_owner_task() const;
    TaskHandle_t owner_task_ = nullptr;
    camera_fb_t* outstanding_ = nullptr;
    bool initialized_ = false;
};
