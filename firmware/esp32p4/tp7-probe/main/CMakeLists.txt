idf_component_register(
    SRCS probe_main.cpp UsbClient.cpp PtpSession.cpp Tp7Midi.cpp
    INCLUDE_DIRS .
    PRIV_REQUIRES usb esp_driver_gpio esp_timer)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20 -Wall -Wextra -Werror)
