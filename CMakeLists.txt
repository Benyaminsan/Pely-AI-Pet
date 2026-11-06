cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET "esp32s3" CACHE STRING "Pely firmware target")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pely_ai_pet)
