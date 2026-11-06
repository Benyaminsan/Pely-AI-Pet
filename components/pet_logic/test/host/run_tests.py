"""Compile the production C++ component against a small deterministic ESP-IDF fake."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


FAKE_IDF = r"""
#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NO_MEM = 0x101;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
constexpr esp_err_t ESP_ERR_INVALID_SIZE = 0x104;
constexpr esp_err_t ESP_ERR_NOT_SUPPORTED = 0x106;
constexpr esp_err_t ESP_ERR_TIMEOUT = 0x107;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
using TickType_t = uint32_t;
using BaseType_t = int;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
struct FakeMutex;
using SemaphoreHandle_t = FakeMutex*;
SemaphoreHandle_t xSemaphoreCreateMutex();
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
BaseType_t xPortGetCoreID();
int64_t esp_timer_get_time();
uint32_t esp_random();
using esp_event_base_t = const char*;
using esp_event_loop_handle_t = void*;
#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t const id
#define ESP_EVENT_DEFINE_BASE(id) esp_event_base_t const id = #id
esp_err_t esp_event_post_to(esp_event_loop_handle_t, esp_event_base_t, int32_t,
                             const void*, std::size_t, TickType_t);
using nvs_handle_t = uint32_t;
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, std::size_t*);
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, std::size_t);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_commit(nvs_handle_t);
"""


def main():
    default_compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", nargs="+", default=[default_compiler] if default_compiler else None,
                        help="Compiler command (e.g. clang++, or /path/to/zig c++)")
    args = parser.parse_args()
    if not args.compiler:
        parser.error("No GCC/Clang compiler found; provide --compiler PATH [ARG ...]")
    component = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="pet-logic-test-") as directory:
        output = Path(directory)
        (output / "fake_idf.hpp").write_text(FAKE_IDF, encoding="utf-8")
        for name in ("esp_err.h", "esp_event.h", "nvs.h", "esp_random.h", "esp_timer.h",
                     "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h"):
            header = output / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "fake_idf.hpp"\n', encoding="utf-8")
        executable = output / "pet_logic_tests.exe"
        command = [*args.compiler, "-std=c++17", "-fno-exceptions", "-fno-rtti",
                   "-Wall", "-Wextra", "-Werror", "-Wformat=2",
                   "-pedantic", "-I", str(output), "-I", str(component / "include"),
                   str(component / "pet_logic.cpp"), str(Path(__file__).with_name("pet_logic_test.cpp")),
                   "-o", str(executable)]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
