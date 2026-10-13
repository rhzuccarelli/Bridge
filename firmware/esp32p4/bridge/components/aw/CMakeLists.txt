##
## The portable Bridge layers plus the esp32p4 platform assembly, as one
## ESP-IDF component.
##
## idf_component_register sets ESP-IDF's compile options, definitions and
## common link libraries on this directory, so the plain CMake targets that
## add_subdirectory creates below inherit them. That is what lets the existing
## src/*/CMakeLists.txt files build unchanged for the device.
##
idf_component_register(REQUIRES esp_timer esp_driver_gpio esp_driver_spi usb fatfs sdmmc
                                esp_driver_sdmmc pthread)

cmake_path(SET AW_ROOT NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/../../../../..")
set(AW_PLATFORM "esp32p4" CACHE STRING "Hardware backend to build" FORCE)
list(APPEND CMAKE_MODULE_PATH "${AW_ROOT}/cmake")
include(AwPlatform)
include(AwCompilerOptions)

add_subdirectory("${AW_ROOT}/src" aw)

target_link_libraries(${COMPONENT_LIB} INTERFACE aw::app aw::platform)
