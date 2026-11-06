"""Compile both production components with deterministic ESP-DL/camera facades."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


FAKE_IDF = r"""
#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=0x101, ESP_ERR_INVALID_ARG=0x102;
constexpr int ESP_ERR_INVALID_STATE=0x103, ESP_ERR_INVALID_SIZE=0x104;
constexpr int ESP_ERR_NOT_SUPPORTED=0x106, ESP_ERR_TIMEOUT=0x107, ESP_ERR_INVALID_RESPONSE=0x108;
using TickType_t=uint32_t;
using TaskHandle_t=void*;
using esp_event_base_t=const char*;
using esp_event_loop_handle_t=void*;
#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t const id
#define ESP_EVENT_DEFINE_BASE(id) esp_event_base_t const id=#id
#define configASSERT(condition) assert(condition)
#define CONFIG_CAMERA_CORE1 1
#define GPIO_IS_VALID_GPIO(pin) ((pin)>=0 && (pin)<49)
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) GPIO_IS_VALID_GPIO(pin)
constexpr uint32_t MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2;
int xPortGetCoreID();
TaskHandle_t xTaskGetCurrentTaskHandle();
int64_t esp_timer_get_time();
std::size_t heap_caps_get_total_size(uint32_t);
void* heap_caps_malloc(std::size_t,uint32_t);
void* heap_caps_aligned_alloc(std::size_t,std::size_t,uint32_t);
void heap_caps_free(void*);
esp_err_t esp_event_post_to(esp_event_loop_handle_t,esp_event_base_t,int32_t,
                            const void*,std::size_t,TickType_t);
enum framesize_t { FRAMESIZE_96X96, FRAMESIZE_QQVGA, FRAMESIZE_QCIF,
                  FRAMESIZE_HQVGA, FRAMESIZE_240X240, FRAMESIZE_QVGA, FRAMESIZE_VGA };
enum pixformat_t { PIXFORMAT_RGB565, PIXFORMAT_JPEG };
enum ledc_timer_t { LEDC_TIMER_0 };
enum ledc_channel_t { LEDC_CHANNEL_0 };
enum camera_fb_location_t { CAMERA_FB_IN_PSRAM, CAMERA_FB_IN_DRAM };
enum camera_grab_mode_t { CAMERA_GRAB_WHEN_EMPTY, CAMERA_GRAB_LATEST };
constexpr int OV5640_PID=0x5640;
struct sensor_t {
    struct { int PID=OV5640_PID; } id;
    int (*set_hmirror)(sensor_t*,int)=nullptr;
    int (*set_vflip)(sensor_t*,int)=nullptr;
};
struct camera_config_t {
    int pin_pwdn, pin_reset, pin_xclk, pin_sccb_sda, pin_sccb_scl;
    int pin_d7,pin_d6,pin_d5,pin_d4,pin_d3,pin_d2,pin_d1,pin_d0;
    int pin_vsync,pin_href,pin_pclk,xclk_freq_hz;
    ledc_timer_t ledc_timer;
    ledc_channel_t ledc_channel;
    pixformat_t pixel_format;
    framesize_t frame_size;
    std::size_t fb_count;
    camera_fb_location_t fb_location;
    camera_grab_mode_t grab_mode;
};
struct camera_fb_t {
    uint8_t* buf=nullptr;
    std::size_t len=0,width=0,height=0;
    pixformat_t format=PIXFORMAT_RGB565;
};
esp_err_t esp_camera_init(const camera_config_t*);
esp_err_t esp_camera_deinit();
sensor_t* esp_camera_sensor_get();
camera_fb_t* esp_camera_fb_get();
void esp_camera_fb_return(camera_fb_t*);
namespace fbs { enum model_location_type_t { MODEL_LOCATION_IN_FLASH_RODATA }; }
namespace dl {
enum dtype_t { DATA_TYPE_INT8=3, DATA_TYPE_FLOAT=1 };
enum memory_manager_t { MEMORY_MANAGER_GREEDY };
enum runtime_mode_t { RUNTIME_MODE_SINGLE_CORE, RUNTIME_MODE_DUAL_CORE };
struct ExponentInfo {
    int value=-7;
    bool valid=true,per_channel=false;
    operator int() const { return value; }
    bool is_valid() const { return valid; }
    bool is_per_channel() const { return per_channel; }
};
class TensorBase {
public:
    std::vector<int> shape;
    dtype_t dtype=DATA_TYPE_INT8;
    ExponentInfo exponent;
    std::vector<int8_t> storage;
    int get_size() { return static_cast<int>(storage.size()); }
    void* get_element_ptr() { return storage.data(); }
    template<class T> T* get_element_ptr() { return reinterpret_cast<T*>(storage.data()); }
};
class Model {
public:
    Model(const char*,fbs::model_location_type_t,int,memory_manager_t,const uint8_t*,bool);
    virtual ~Model();
    virtual std::map<std::string,TensorBase*>& get_inputs() { return inputs; }
    virtual std::map<std::string,TensorBase*>& get_outputs() { return outputs; }
    virtual void run(runtime_mode_t);
private:
    TensorBase input,output;
    std::map<std::string,TensorBase*> inputs,outputs;
};
}
"""

# Test fixture only: the fake Model supplies tensor data; these are not model weights.
MODEL_BYTES = "'E','D','L','2',0,0,0,0,16,0,0,0,0,0,0,0," + ",".join(["0"] * 16)
MODEL_HEADER = "#pragma once\nalignas(16) const unsigned char fomo_model_int8[] = {" + MODEL_BYTES + "};\n"


def main():
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", nargs="+", default=[compiler] if compiler else None)
    args = parser.parse_args()
    if not args.compiler:
        parser.error("Provide a GCC/Clang compiler using --compiler PATH [ARG ...]")
    component = Path(__file__).resolve().parents[2]
    camera = component.parent / "camera_hal"
    with tempfile.TemporaryDirectory(prefix="vision-ai-test-") as directory:
        output = Path(directory)
        (output / "fake_idf.hpp").write_text(FAKE_IDF, encoding="utf-8")
        for name in ("esp_err.h", "esp_event.h", "esp_camera.h", "esp_heap_caps.h", "esp_timer.h",
                     "sdkconfig.h", "driver/gpio.h", "freertos/FreeRTOS.h", "freertos/task.h",
                     "dl_model_base.hpp", "dl_tensor_base.hpp"):
            header = output / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "fake_idf.hpp"\n', encoding="utf-8")
        executable = output / "vision_ai_tests.exe"
        command = [*args.compiler, "-std=c++17", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra",
                   "-Werror", "-Wformat=2", "-pedantic", "-I", str(output),
                   "-I", str(component / "include"), "-I", str(camera / "include"),
                   str(component / "vision_ai.cpp"), str(camera / "camera_hal.cpp"),
                   str(Path(__file__).with_name("vision_ai_test.cpp")), "-o", str(executable)]
        variants = [
            ("normal", MODEL_HEADER, []),
            ("unaligned array", "#pragma once\nstruct alignas(16) Fixture { unsigned char pad; "
             "unsigned char data[32]; };\nconst Fixture fixture = {0,{" + MODEL_BYTES + "}};\n"
             "#define fomo_model_int8 fixture.data\n", ["unaligned"]),
            ("TFLite array rejected", MODEL_HEADER.replace("'E','D','L','2'", "'T','F','L','3'"), ["format"]),
            ("truncated model rejected", MODEL_HEADER.replace("16,0,0,0", "255,0,0,0", 1), ["size"]),
        ]
        for name, header, arguments in variants:
            print("Fixture:", name, flush=True)
            (output / "fomo_model.h").write_text(header, encoding="utf-8")
            subprocess.run(command, check=True)
            subprocess.run([str(executable), *arguments], check=True)


if __name__ == "__main__":
    main()
