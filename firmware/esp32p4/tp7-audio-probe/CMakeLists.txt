##
## TP-7 USB monitor-audio probe for the ESP32-P4 (port step P3a).
##
## A stand-alone ESP-IDF project, like tp7-probe: it answers "can the P4 stream
## the TP-7's high-bandwidth UAC1 playback endpoint" before an audio backend is
## written against it.
##
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(tp7-audio-probe)
