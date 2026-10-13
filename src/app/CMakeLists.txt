##
## audio-workstation - the executable.
##
## `Application` is a library so tests can construct it against a test platform;
## `main.cpp` is the only piece that cannot be tested, and it contains nothing
## but wiring.
##
add_library(aw_app STATIC Application.cpp)
add_library(aw::app ALIAS aw_app)

target_include_directories(aw_app PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_app
    PUBLIC aw::core aw::hal aw::events aw::display aw::audio aw::project aw::mtp aw::ui
           aw::compiler_options)

# On the ESP32-P4 the composition root is app_main in firmware/esp32p4/bridge.
if(NOT AW_PLATFORM STREQUAL "esp32p4")
    add_executable(audio-workstation main.cpp)
    target_link_libraries(audio-workstation PRIVATE aw::app aw::platform)
endif()
