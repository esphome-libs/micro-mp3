# ESP32-S3 MP3 Decode Benchmark Example

cmake_minimum_required(VERSION 3.16)

# Include micro-mp3 component from parent directory
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(decode_benchmark)
