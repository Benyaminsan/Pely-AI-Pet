"""Compile CloudSync + PetLogic with real ESP-IDF cJSON and fake NVS/Wi-Fi/RTOS."""

import argparse
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


EXTRA_IDF = r"""
#include <cassert>
using TaskHandle_t=void*;
using EventBits_t=uint32_t;
using UBaseType_t=unsigned;
using esp_event_handler_instance_t=void*;
using esp_event_handler_t=void(*)(void*,esp_event_base_t,int32_t,void*);
struct FakeEventGroup;
using EventGroupHandle_t=FakeEventGroup*;
constexpr int pdTRUE=1,pdFALSE=0,pdPASS=1,ESP_EVENT_ANY_ID=-1;
#define configASSERT(x) assert(x)
#define pdMS_TO_TICKS(ms) static_cast<TickType_t>(ms)
TaskHandle_t xTaskGetCurrentTaskHandle();
BaseType_t xTaskCreatePinnedToCore(void(*)(void*),const char*,uint32_t,void*,UBaseType_t,TaskHandle_t*,BaseType_t);
void vTaskDelete(TaskHandle_t);
EventGroupHandle_t xEventGroupCreate();
void vEventGroupDelete(EventGroupHandle_t);
EventBits_t xEventGroupSetBits(EventGroupHandle_t,EventBits_t);
EventBits_t xEventGroupGetBits(EventGroupHandle_t);
EventBits_t xEventGroupClearBits(EventGroupHandle_t,EventBits_t);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t,EventBits_t,BaseType_t,BaseType_t,TickType_t);
esp_err_t nvs_flash_init();
enum esp_mac_type_t { ESP_MAC_WIFI_STA };
esp_err_t esp_read_mac(uint8_t*,esp_mac_type_t);
ESP_EVENT_DECLARE_BASE(IP_EVENT);
ESP_EVENT_DECLARE_BASE(WIFI_EVENT);
constexpr int IP_EVENT_STA_GOT_IP=1,IP_EVENT_STA_LOST_IP=2,WIFI_EVENT_STA_DISCONNECTED=3,WIFI_EVENT_STA_STOP=4;
struct esp_netif_t {};
struct esp_netif_ip_info_t { struct { uint32_t addr=0; } ip; };
struct ip_event_got_ip_t { esp_netif_t* esp_netif; esp_netif_ip_info_t ip_info; };
esp_netif_t* esp_netif_get_handle_from_ifkey(const char*);
bool esp_netif_is_netif_up(esp_netif_t*);
esp_err_t esp_netif_get_ip_info(esp_netif_t*,esp_netif_ip_info_t*);
esp_err_t esp_event_handler_instance_register(esp_event_base_t,int32_t,esp_event_handler_t,void*,esp_event_handler_instance_t*);
esp_err_t esp_event_handler_instance_register_with(esp_event_loop_handle_t,esp_event_base_t,int32_t,esp_event_handler_t,void*,esp_event_handler_instance_t*);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t,int32_t,esp_event_handler_instance_t);
esp_err_t esp_event_handler_instance_unregister_with(esp_event_loop_handle_t,esp_event_base_t,int32_t,esp_event_handler_instance_t);
"""


def main():
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", nargs="+", default=[compiler] if compiler else None)
    parser.add_argument("--cjson-dir", type=Path,
                        default=Path(os.environ["IDF_PATH"]) / "components/json/cJSON" if os.environ.get("IDF_PATH") else None,
                        help="Directory containing cJSON.c and cJSON.h (ESP-IDF bundled source)")
    args = parser.parse_args()
    if not args.compiler or not args.cjson_dir:
        parser.error("Provide a compiler and --cjson-dir, or set IDF_PATH")
    cjson = args.cjson_dir.resolve()
    if not (cjson / "cJSON.c").is_file() or not (cjson / "cJSON.h").is_file():
        parser.error("cJSON.c/cJSON.h not found")
    cloud = Path(__file__).resolve().parents[2]
    pet = cloud.parent / "pet_logic"
    spec = importlib.util.spec_from_file_location("pet_host_runner", pet / "test/host/run_tests.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory(prefix="cloud-sync-test-") as directory:
        output = Path(directory)
        (output / "fake_idf.hpp").write_text(module.FAKE_IDF + EXTRA_IDF, encoding="utf-8")
        for name in ("esp_err.h", "esp_event.h", "nvs.h", "nvs_flash.h", "esp_random.h", "esp_timer.h",
                     "esp_mac.h", "esp_netif.h", "esp_wifi.h", "freertos/FreeRTOS.h", "freertos/semphr.h",
                     "freertos/task.h", "freertos/event_groups.h"):
            header = output / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "fake_idf.hpp"\n', encoding="utf-8")
        obj = output / "cJSON.o"
        subprocess.run([*args.compiler, "-x", "c", "-std=c99", "-c", str(cjson / "cJSON.c"), "-o", str(obj)], check=True)
        executable = output / "cloud_sync_tests.exe"
        command = [*args.compiler, "-std=c++17", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror",
                   "-Wformat=2", "-pedantic", "-I", str(output), "-I", str(cloud / "include"),
                   "-I", str(pet / "include"), "-I", str(cjson), str(cloud / "cloud_sync.cpp"),
                   str(pet / "pet_logic.cpp"), str(Path(__file__).with_name("cloud_sync_test.cpp")),
                   str(obj), "-o", str(executable)]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
