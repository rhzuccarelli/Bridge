##
## Bridge firmware for the ESP32-P4.
##
## The application itself is not here: components/aw adds the repository's
## src/ tree, so the device builds the same per-layer libraries, with the same
## layering rules, as the Raspberry Pi and the simulator. This project adds only
## the ESP-IDF configuration and app_main.
##
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(bridge)
