# UsbClient (client task, control transfers) is shared with the P0 probe.
idf_component_register(
    SRCS audio_probe_main.cpp ../../tp7-probe/main/UsbClient.cpp
    INCLUDE_DIRS . ../../tp7-probe/main
    PRIV_REQUIRES usb soc esp_driver_gpio esp_timer)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20 -Wall -Wextra -Werror)
