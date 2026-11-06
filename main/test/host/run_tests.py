"""Compile the real entry point and component headers with host device/RTOS endpoints."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


SDK = r"""
#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NO_MEM=0x101,ESP_ERR_INVALID_ARG=0x102;
constexpr int ESP_ERR_INVALID_STATE=0x103,ESP_ERR_INVALID_SIZE=0x104,ESP_ERR_TIMEOUT=0x107;
using TickType_t=uint32_t;
using BaseType_t=int;
using UBaseType_t=unsigned;
using TaskHandle_t=void*;
using TaskFunction_t=void(*)(void*);
using EventBits_t=uint32_t;
using nvs_handle_t=uint32_t;
using esp_event_base_t=const char*;
using esp_event_loop_handle_t=void*;
using esp_event_handler_instance_t=void*;
using esp_event_handler_t=void(*)(void*,esp_event_base_t,int32_t,void*);
struct MockQueue;
struct MockGroup;
struct MockMutex;
using QueueHandle_t=MockQueue*;
using EventGroupHandle_t=MockGroup*;
using SemaphoreHandle_t=MockMutex*;
struct portMUX_TYPE { int depth=0; };
#define portMUX_INITIALIZER_UNLOCKED {}
constexpr int pdTRUE=1,pdFALSE=0,pdPASS=1,ESP_EVENT_ANY_ID=-1;
constexpr TickType_t portMAX_DELAY=UINT32_MAX;
#define pdMS_TO_TICKS(ms) static_cast<TickType_t>((ms)/10)
#define configASSERT(x) assert(x)
#define DRAM_ATTR
#define IRAM_ATTR
#define ESP_INTR_FLAG_IRAM 1
#define ESP_EVENT_DECLARE_BASE(name) extern esp_event_base_t const name
#define ESP_EVENT_DEFINE_BASE(name) esp_event_base_t const name=#name
void mock_error(esp_err_t,const char*);
void mock_log(const char*,const char*,...) __attribute__((format(printf,2,3)));
#define ESP_ERROR_CHECK(x) mock_error((x),#x)
#define ESP_LOGI(...) mock_log(__VA_ARGS__)
#define ESP_LOGW(...) mock_log(__VA_ARGS__)
#define ESP_LOGE(...) mock_log(__VA_ARGS__)
const char* esp_err_to_name(esp_err_t);
int xPortGetCoreID();
TaskHandle_t xTaskGetCurrentTaskHandle();
TickType_t xTaskGetTickCount();
void vTaskDelay(TickType_t);
void vTaskDelayUntil(TickType_t*,TickType_t);
void vTaskDelete(TaskHandle_t);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t,const char*,uint32_t,void*,UBaseType_t,TaskHandle_t*,BaseType_t);
QueueHandle_t xQueueCreate(unsigned,std::size_t);
BaseType_t xQueueOverwrite(QueueHandle_t,const void*);
BaseType_t xQueueReceive(QueueHandle_t,void*,TickType_t);
EventGroupHandle_t xEventGroupCreate();
EventBits_t xEventGroupSetBits(EventGroupHandle_t,EventBits_t);
EventBits_t xEventGroupClearBits(EventGroupHandle_t,EventBits_t);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t,EventBits_t,BaseType_t,BaseType_t,TickType_t);
int64_t esp_timer_get_time();
esp_err_t nvs_flash_init();
struct esp_event_loop_args_t {
    int32_t queue_size;
    const char* task_name;
    UBaseType_t task_priority;
    uint32_t task_stack_size;
    BaseType_t task_core_id;
};
esp_err_t esp_event_loop_create_default();
esp_err_t esp_event_loop_create(const esp_event_loop_args_t*,esp_event_loop_handle_t*);
esp_err_t esp_event_loop_run(esp_event_loop_handle_t,TickType_t);
esp_err_t esp_event_handler_register_with(esp_event_loop_handle_t,esp_event_base_t,int32_t,esp_event_handler_t,void*);
esp_err_t esp_event_handler_register(esp_event_base_t,int32_t,esp_event_handler_t,void*);
esp_err_t esp_event_post_to(esp_event_loop_handle_t,esp_event_base_t,int32_t,const void*,std::size_t,TickType_t);
esp_err_t gpio_install_isr_service(int);
enum framesize_t { FRAMESIZE_QVGA };
enum ledc_timer_t { LEDC_TIMER_0 };
enum ledc_channel_t { LEDC_CHANNEL_0 };
struct camera_fb_t { uint8_t* buf; std::size_t len,width,height; };
struct led_strip_t;
using led_strip_handle_t=led_strip_t*;
struct esp_netif_t {};
esp_err_t esp_netif_init();
esp_netif_t* esp_netif_create_default_wifi_sta();
ESP_EVENT_DECLARE_BASE(WIFI_EVENT);
ESP_EVENT_DECLARE_BASE(IP_EVENT);
constexpr int WIFI_EVENT_STA_START=1,WIFI_EVENT_STA_DISCONNECTED=2,IP_EVENT_STA_GOT_IP=3;
enum wifi_auth_mode_t { WIFI_AUTH_OPEN,WIFI_AUTH_WPA2_PSK };
enum wifi_storage_t { WIFI_STORAGE_RAM };
enum wifi_mode_t { WIFI_MODE_STA };
enum wifi_interface_t { WIFI_IF_STA };
struct wifi_config_t { struct { uint8_t ssid[32],password[64]; struct { wifi_auth_mode_t authmode; } threshold; } sta; };
struct wifi_init_config_t { int marker; };
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{1}
esp_err_t esp_wifi_init(const wifi_init_config_t*);
esp_err_t esp_wifi_set_storage(wifi_storage_t);
esp_err_t esp_wifi_set_mode(wifi_mode_t);
esp_err_t esp_wifi_set_config(wifi_interface_t,const wifi_config_t*);
esp_err_t esp_wifi_start();
esp_err_t esp_wifi_connect();
"""

CONFIG = r"""
#pragma once
#define CONFIG_PELY_CAM_PWDN -1
#define CONFIG_PELY_CAM_RESET -1
#define CONFIG_PELY_CAM_XCLK 2
#define CONFIG_PELY_CAM_SDA 3
#define CONFIG_PELY_CAM_SCL 4
#define CONFIG_PELY_CAM_D0 5
#define CONFIG_PELY_CAM_D1 6
#define CONFIG_PELY_CAM_D2 7
#define CONFIG_PELY_CAM_D3 8
#define CONFIG_PELY_CAM_D4 9
#define CONFIG_PELY_CAM_D5 10
#define CONFIG_PELY_CAM_D6 11
#define CONFIG_PELY_CAM_D7 12
#define CONFIG_PELY_CAM_VSYNC 13
#define CONFIG_PELY_CAM_HREF 14
#define CONFIG_PELY_CAM_PCLK 15
#define CONFIG_PELY_FOMO_LABEL_IDS "apple, milk"
#define CONFIG_PELY_FOMO_BACKGROUND_CHANNEL 0
#define CONFIG_PELY_FOMO_INPUT_WIDTH 96
#define CONFIG_PELY_FOMO_INPUT_HEIGHT 96
#define CONFIG_PELY_CONFIDENCE_PERCENT 70
#define CONFIG_PELY_CONFIRM_FRAMES 3
#define CONFIG_PELY_VISION_MAX_AGE_MS 1000
#define CONFIG_PELY_LED_GPIO 18
#define CONFIG_PELY_LED_WIDTH 16
#define CONFIG_PELY_LED_HEIGHT 8
#define CONFIG_PELY_LED_TILE_WIDTH 0
#define CONFIG_PELY_LED_TILE_HEIGHT 0
#define CONFIG_PELY_LED_BRIGHTNESS 24
#define CONFIG_PELY_LED_SERPENTINE 1
#define CONFIG_PELY_TOUCH_CHANNEL 7
#define CONFIG_PELY_TOUCH_ACTIVE_HIGH 1
#ifdef PELY_TEST_INTERNAL_TOUCH
#define CONFIG_PELY_TOUCH_INTERNAL 1
#else
#define CONFIG_PELY_TOUCH_TTP223 1
#define CONFIG_PELY_TOUCH_GPIO 4
#endif
#ifdef PELY_TEST_WIFI
#define CONFIG_PELY_WIFI_ENABLED 1
#define CONFIG_PELY_WIFI_SSID "12345678901234567890123456789012"
#define CONFIG_PELY_WIFI_PASSWORD "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define CONFIG_PELY_WIFI_MAX_RETRIES 3
#endif
"""


def main():
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", nargs="+", default=[compiler] if compiler else None)
    parser.add_argument("--cjson-dir", type=Path,
                        default=Path(os.environ["IDF_PATH"]) / "components/json/cJSON" if os.environ.get("IDF_PATH") else None)
    args = parser.parse_args()
    if not args.compiler or not args.cjson_dir:
        parser.error("Provide --compiler and --cjson-dir (or set IDF_PATH)")
    app = Path(__file__).resolve().parents[2]
    root = app.parent
    cjson = args.cjson_dir.resolve()
    with tempfile.TemporaryDirectory(prefix="pely-main-test-") as directory:
        output = Path(directory)
        (output / "mock_sdk.hpp").write_text(SDK, encoding="utf-8")
        (output / "sdkconfig.h").write_text(CONFIG, encoding="utf-8")
        for name in ("esp_err.h", "esp_event.h", "esp_attr.h", "esp_log.h", "esp_intr_alloc.h",
                     "esp_netif.h", "esp_timer.h", "esp_wifi.h", "esp_camera.h", "led_strip.h", "nvs.h",
                     "nvs_flash.h", "driver/gpio.h", "freertos/FreeRTOS.h", "freertos/task.h",
                     "freertos/semphr.h", "freertos/queue.h", "freertos/event_groups.h"):
            header = output / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "mock_sdk.hpp"\n', encoding="utf-8")
        text = (root / "data/food_catalog.json").read_text(encoding="utf-8")
        assert ')PELYJSON"' not in text
        (output / "embedded_catalog.cpp").write_text(
            'extern const char food_catalog_json_start[] asm("_binary_food_catalog_json_start") = R"PELYJSON('
            + text + ')PELYJSON";\n', encoding="utf-8")
        obj = output / "cJSON.o"
        subprocess.run([*args.compiler, "-x", "c", "-std=c99", "-c", str(cjson / "cJSON.c"), "-o", str(obj)], check=True)
        includes = ["-I", str(output), "-I", str(cjson), "-I", str(app)]
        for name in ("pet_logic", "camera_hal", "vision_ai", "led_matrix", "touch_sensor", "cloud_sync"):
            includes += ["-I", str(root / "components" / name / "include")]
        for name, flags in (("offline", []), ("wifi_assertions_disabled", ["-DPELY_TEST_WIFI", "-DNDEBUG"]),
                            ("internal_touch", ["-DPELY_TEST_INTERNAL_TOUCH"])):
            print("Configuration:", name, flush=True)
            executable = output / (name + ".exe")
            subprocess.run([*args.compiler, "-std=c++17", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra",
                            "-Werror", "-Wformat=2", "-Wundef", "-pedantic", *flags, *includes,
                            str(Path(__file__).with_name("main_runtime_test.cpp")), str(output / "embedded_catalog.cpp"),
                            str(obj), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
