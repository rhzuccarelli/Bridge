##
## TP-7 USB bring-up probe for the ESP32-P4.
##
## A stand-alone ESP-IDF project, deliberately separate from the Bridge CMake
## build: it exists to answer "can this board see, switch and talk MTP to a
## TP-7" before any of the portable layers are ported.
##
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(tp7-probe)
