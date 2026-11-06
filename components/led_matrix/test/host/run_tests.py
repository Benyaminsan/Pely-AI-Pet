"""Compile the touch/eye components with deterministic interrupt and RMT facades."""

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
using esp_err_t=int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=0x101, ESP_ERR_INVALID_ARG=0x102;
constexpr int ESP_ERR_INVALID_STATE=0x103, ESP_ERR_TIMEOUT=0x107, ESP_ERR_INVALID_RESPONSE=0x108;
using TickType_t=uint32_t;
using BaseType_t=int;
using TaskHandle_t=void*;
using esp_event_loop_handle_t=void*;
using esp_event_base_t=const char*;
constexpr TickType_t portMAX_DELAY=UINT32_MAX;
constexpr int pdTRUE=1,pdFALSE=0;
#define FAKE_STRING_IMPL(value) #value
#define FAKE_STRING(value) FAKE_STRING_IMPL(value)
#define IRAM_ATTR __attribute__((section(".iram1." FAKE_STRING(__COUNTER__))))
#define ESP_INTR_FLAG_IRAM 1
#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t const id
#define ESP_EVENT_DEFINE_BASE(id) esp_event_base_t const id=#id
#define configASSERT(x) assert(x)
#define pdMS_TO_TICKS(ms) static_cast<TickType_t>(ms)
struct portMUX_TYPE { int depth; };
#define portMUX_INITIALIZER_UNLOCKED {0}
void fake_enter(portMUX_TYPE*,bool);
void fake_exit(portMUX_TYPE*,bool);
void fake_yield();
#define portENTER_CRITICAL(p) fake_enter(p,false)
#define portEXIT_CRITICAL(p) fake_exit(p,false)
#define portENTER_CRITICAL_ISR(p) fake_enter(p,true)
#define portEXIT_CRITICAL_ISR(p) fake_exit(p,true)
#define portYIELD_FROM_ISR() fake_yield()
struct FakeQueue;
struct FakeMutex;
using QueueHandle_t=FakeQueue*;
using SemaphoreHandle_t=FakeMutex*;
QueueHandle_t xQueueCreate(unsigned,std::size_t);
void vQueueDelete(QueueHandle_t);
BaseType_t xQueueSendFromISR(QueueHandle_t,const void*,BaseType_t*);
BaseType_t xQueueSendToFront(QueueHandle_t,const void*,TickType_t);
BaseType_t xQueueReceive(QueueHandle_t,void*,TickType_t);
SemaphoreHandle_t xSemaphoreCreateMutex();
BaseType_t xSemaphoreTake(SemaphoreHandle_t,TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
int xPortGetCoreID();
TaskHandle_t xTaskGetCurrentTaskHandle();
TickType_t xTaskGetTickCount();
void vTaskDelay(TickType_t);
int64_t esp_timer_get_time();
uint32_t esp_random();
bool esp_ptr_internal(const void*);
constexpr uint32_t MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2;
void* heap_caps_calloc(std::size_t,std::size_t,uint32_t);
void heap_caps_free(void*);
esp_err_t esp_event_post_to(esp_event_loop_handle_t,esp_event_base_t,int32_t,
                            const void*,std::size_t,TickType_t);
using gpio_num_t=int;
using gpio_isr_t=void(*)(void*);
enum gpio_int_type_t { GPIO_INTR_DISABLE,GPIO_INTR_POSEDGE,GPIO_INTR_NEGEDGE };
enum gpio_mode_t { GPIO_MODE_INPUT };
struct gpio_config_t {
    uint64_t pin_bit_mask=0;
    gpio_mode_t mode=GPIO_MODE_INPUT;
    gpio_int_type_t intr_type=GPIO_INTR_DISABLE;
};
#define GPIO_IS_VALID_GPIO(n) ((n)>=0 && (n)<49)
#define GPIO_IS_VALID_OUTPUT_GPIO(n) (GPIO_IS_VALID_GPIO(n) && (n)!=46)
esp_err_t gpio_config(const gpio_config_t*);
esp_err_t gpio_install_isr_service(int);
esp_err_t gpio_isr_handler_add(gpio_num_t,gpio_isr_t,void*);
esp_err_t gpio_isr_handler_remove(gpio_num_t);
esp_err_t gpio_set_intr_type(gpio_num_t,gpio_int_type_t);
esp_err_t gpio_intr_enable(gpio_num_t);
esp_err_t gpio_intr_disable(gpio_num_t);
esp_err_t gpio_reset_pin(gpio_num_t);
using touch_pad_t=int;
enum touch_pad_intr_mask_t { TOUCH_PAD_INTR_MASK_ACTIVE=1,TOUCH_PAD_INTR_MASK_INACTIVE=2,TOUCH_PAD_INTR_MASK_TIMEOUT=4 };
enum touch_filter_mode_t { TOUCH_PAD_FILTER_IIR_16 };
enum touch_smooth_mode_t { TOUCH_PAD_SMOOTH_IIR_2 };
enum touch_fsm_mode_t { TOUCH_FSM_MODE_TIMER };
constexpr uint32_t TOUCH_PAD_THRESHOLD_MAX=0x1fffff;
struct touch_filter_config_t {
    touch_filter_mode_t mode;
    unsigned debounce_cnt,noise_thr,jitter_step;
    touch_smooth_mode_t smh_lvl;
};
esp_err_t touch_pad_init();
esp_err_t touch_pad_deinit();
esp_err_t touch_pad_config(touch_pad_t);
esp_err_t touch_pad_set_thresh(touch_pad_t,uint32_t);
esp_err_t touch_pad_filter_set_config(const touch_filter_config_t*);
esp_err_t touch_pad_filter_enable();
esp_err_t touch_pad_set_fsm_mode(touch_fsm_mode_t);
esp_err_t touch_pad_fsm_start();
esp_err_t touch_pad_fsm_stop();
esp_err_t touch_pad_read_benchmark(touch_pad_t,uint32_t*);
esp_err_t touch_pad_timeout_set(bool,uint32_t);
esp_err_t touch_pad_isr_register(void(*)(void*),void*,touch_pad_intr_mask_t);
esp_err_t touch_pad_isr_deregister(void(*)(void*),void*);
esp_err_t touch_pad_intr_enable(touch_pad_intr_mask_t);
esp_err_t touch_pad_intr_disable(touch_pad_intr_mask_t);
esp_err_t touch_pad_intr_clear(touch_pad_intr_mask_t);
uint32_t touch_pad_read_intr_status_mask();
touch_pad_t touch_pad_get_current_meas_channel();
esp_err_t touch_pad_timeout_resume();
enum rmt_clock_source_t { RMT_CLK_SRC_DEFAULT };
using spi_clock_source_t=int;
using spi_host_device_t=int;
"""

FAKE_LED = r"""
#pragma once
#include "fake_idf.hpp"
typedef struct led_strip_t* led_strip_handle_t;
enum led_model_t { LED_MODEL_WS2812 };
union led_color_component_format_t {
    struct { uint32_t r_pos:2,g_pos:2,b_pos:2,w_pos:2,reserved:19,bytes_per_color:2,num_components:3; } format;
    uint32_t format_id;
};
struct led_strip_config_t {
    int strip_gpio_num;
    uint32_t max_leds;
    led_model_t led_model;
    led_color_component_format_t color_component_format;
};
struct led_strip_rmt_config_t {
    rmt_clock_source_t clk_src;
    uint32_t resolution_hz;
    std::size_t mem_block_symbols;
    struct { uint32_t with_dma:1; } flags;
};
extern "C" {
esp_err_t led_strip_new_rmt_device(const led_strip_config_t*,const led_strip_rmt_config_t*,led_strip_handle_t*);
esp_err_t led_strip_set_pixel(led_strip_handle_t,uint32_t,uint32_t,uint32_t,uint32_t);
esp_err_t led_strip_refresh(led_strip_handle_t);
esp_err_t led_strip_clear(led_strip_handle_t);
esp_err_t led_strip_del(led_strip_handle_t);
}
"""


def main():
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", nargs="+", default=[compiler] if compiler else None)
    parser.add_argument("--led-strip-headers", type=Path,
                        help="Optional real led_strip 3.0.3 include directory for API checking")
    parser.add_argument("--preview", type=Path, help="Optional output PPM contact sheet for the three expressions")
    args = parser.parse_args()
    if not args.compiler:
        parser.error("Provide --compiler PATH [ARG ...]")
    matrix = Path(__file__).resolve().parents[2]
    touch = matrix.parent / "touch_sensor"
    with tempfile.TemporaryDirectory(prefix="touch-led-test-") as directory:
        output = Path(directory)
        (output / "fake_idf.hpp").write_text(FAKE_IDF, encoding="utf-8")
        for name in ("esp_attr.h", "esp_err.h", "esp_event.h", "esp_timer.h", "esp_heap_caps.h",
                     "esp_intr_alloc.h", "esp_memory_utils.h", "esp_random.h", "esp_idf_version.h",
                     "driver/gpio.h", "driver/touch_pad.h", "driver/rmt_types.h", "driver/spi_master.h",
                     "hal/gpio_types.h", "freertos/FreeRTOS.h", "freertos/queue.h",
                     "freertos/task.h", "freertos/semphr.h"):
            header = output / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "fake_idf.hpp"\n', encoding="utf-8")
        includes = ["-I", str(output), "-I", str(matrix / "include"), "-I", str(touch / "include")]
        if args.led_strip_headers:
            includes += ["-I", str(args.led_strip_headers.resolve())]
        else:
            (output / "led_strip.h").write_text(FAKE_LED, encoding="utf-8")
            (output / "led_strip_rmt.h").write_text('#include "led_strip.h"\n', encoding="utf-8")
        executable = output / "touch_led_tests.exe"
        command = [*args.compiler, "-std=c++17", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra",
                   "-Werror", "-Wformat=2", "-pedantic", *includes, str(matrix / "led_matrix.cpp"),
                   str(touch / "touch_sensor.cpp"), str(Path(__file__).with_name("touch_led_test.cpp")),
                   "-o", str(executable)]
        subprocess.run(command, check=True)
        run = [str(executable)]
        if args.preview:
            run.append(str(args.preview.resolve()))
        subprocess.run(run, check=True)


if __name__ == "__main__":
    main()
