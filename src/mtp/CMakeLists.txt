##
## aw_mtp - device connection lifecycle and object tree navigation.
##
## Contains no protocol code: the wire protocol lives behind hal::MTPInterface,
## implemented by libmtp on the Pi and by a TinyUSB initiator on the ESP32.
## Everything here is policy, and is tested against a scripted fake device.
##
add_library(aw_mtp STATIC
    MtpBrowser.cpp
    MtpManager.cpp)

add_library(aw::mtp ALIAS aw_mtp)

target_include_directories(aw_mtp PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_mtp PUBLIC aw::core aw::hal aw::events aw::compiler_options)
