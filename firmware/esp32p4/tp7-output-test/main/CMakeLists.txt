idf_component_register(SRCS output_test_main.cpp REQUIRES aw esp_psram esp_rom esp_timer)
# The backend's own header, for its counters.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../../../src/platform/esp32p4")
# ESP-IDF headers use GNU statement expressions; FreeRTOS macros expand to C casts.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-pedantic -Wno-old-style-cast)
