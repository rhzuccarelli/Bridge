##
## Platform backend selection.
##
## AW_PLATFORM selects which set of hardware implementations gets linked into
## the application binary.  It never affects the portable layers - those are
## compiled exactly once, with exactly the same flags, on every platform.
##
##   simulator : no hardware at all.  Null/loopback implementations of every
##               HAL interface plus POSIX storage.  Builds anywhere, which is
##               what CI and a developer laptop use.
##   rpi       : Raspberry Pi 5 backends (ALSA, libgpiod, SPI display, libmtp).
##   esp32p4   : ESP32-P4 firmware.  Only inside the ESP-IDF build of
##               firmware/esp32p4/bridge, which sets it; see that README.
##
if(NOT DEFINED AW_PLATFORM)
    set(_aw_default_platform "simulator")
    if(EXISTS "/proc/device-tree/model")
        file(READ "/proc/device-tree/model" _aw_dt_model)
        if(_aw_dt_model MATCHES "Raspberry Pi")
            set(_aw_default_platform "rpi")
        endif()
    endif()
    set(AW_PLATFORM "${_aw_default_platform}" CACHE STRING "Hardware backend to build")
endif()

set_property(CACHE AW_PLATFORM PROPERTY STRINGS simulator rpi esp32p4)

if(AW_PLATFORM STREQUAL "esp32p4")
    if(NOT IDF_TARGET STREQUAL "esp32p4")
        message(FATAL_ERROR
            "AW_PLATFORM=esp32p4 builds only under ESP-IDF: "
            "use idf.py in firmware/esp32p4/bridge.")
    endif()
elseif(NOT AW_PLATFORM MATCHES "^(simulator|rpi)$")
    message(FATAL_ERROR
        "AW_PLATFORM='${AW_PLATFORM}' is not implemented. Use 'simulator' or 'rpi'.")
endif()
