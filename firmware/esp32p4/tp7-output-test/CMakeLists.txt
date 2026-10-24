##
## Bench test for the ESP32-P4 TP-7 monitor output (port step P3).
##
## Builds the real backend (src/platform/esp32p4/usb/Tp7AudioOutput) through
## the bridge firmware's aw component, plays a sine through it, and runs the
## record-and-fetch lab loop of tp7-audio-probe: arm and record on the TP-7
## over USB-MIDI, switch it to MTP, send the take to the Mac over the console.
##
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../bridge/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(tp7-output-test)
