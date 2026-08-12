##
## Platform backends.
##
## Exactly one *assembly* is built (selected by AW_PLATFORM) and it is the only
## code allowed to include a vendor header.  The alias `aw::platform` lets the
## application link "the platform" without naming which.
##
## Below the assembly sit two shared backend libraries, used by both:
##   aw_platform_posix : std::filesystem storage
##   aw_platform_dev   : the development I/O backends - Linux framebuffer, evdev
##                       touch, ANSI terminal preview, keyboard input, and the
##                       composite button merger.
##
add_library(aw_platform_posix STATIC posix/PosixStorage.cpp)
add_library(aw::platform_posix ALIAS aw_platform_posix)
target_include_directories(aw_platform_posix PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_platform_posix PUBLIC aw::core aw::hal aw::compiler_options)

find_package(Threads REQUIRED)

##
## Development I/O backends.
##
## These are real drivers, not mocks: FramebufferDisplay drives the HDMI LCD and
## EvdevTouch reads the XPT2046 through the kernel.  They are grouped as "dev"
## because the shipping device uses neither - it has a knob and switches, and its
## panel is driven by the SPI/LCD peripheral.  Keeping them in one library makes
## that distinction visible in the build rather than only in a comment.
##
add_library(aw_platform_dev STATIC
    CompositeButtons.cpp
    CompositeEncoder.cpp
    FallbackAudioOutput.cpp
    NullAudioOutput.cpp
    desktop/DevBackends.cpp
    desktop/KeyboardInput.cpp
    desktop/TerminalDisplay.cpp
    desktop/VirtualPanel.cpp
    linux/EvdevTouch.cpp
    linux/FramebufferDisplay.cpp
    linux/SpiOledBus.cpp
    oled/Ssd1306Display.cpp)
add_library(aw::platform_dev ALIAS aw_platform_dev)
target_include_directories(aw_platform_dev PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_platform_dev
    PUBLIC aw::core aw::hal aw::compiler_options Threads::Threads)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "aw_platform_dev needs Linux (framebuffer + evdev + spidev). "
        "Build with AW_PLATFORM=simulator and AW_DISPLAY=memory elsewhere.")
endif()

# Bring-up tool: draws a geometric test pattern on the OLED to prove the wiring,
# controller choice and orientation before any UI or font exists.
add_executable(aw-oled-test oled/oled_test_main.cpp)
target_link_libraries(aw-oled-test PRIVATE aw::platform_dev aw::core aw::hal)

if(AW_PLATFORM STREQUAL "simulator")

    add_library(aw_platform STATIC simulator/SimulatorPlatform.cpp)
    target_compile_definitions(aw_platform PUBLIC AW_PLATFORM_SIMULATOR=1)

elseif(AW_PLATFORM STREQUAL "rpi")

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
    pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET libgpiod)
    pkg_check_modules(USB REQUIRED IMPORTED_TARGET libusb-1.0)
    pkg_check_modules(MTP REQUIRED IMPORTED_TARGET libmtp)

    add_library(aw_platform STATIC
        rpi/AlsaAudioOutput.cpp
        rpi/AlsaTp7ModeSwitcher.cpp
        rpi/GpioInput.cpp
        rpi/LibMtpDevice.cpp
        rpi/LibUsbHost.cpp
        rpi/RpiPlatform.cpp
        rpi/SpiDisplay.cpp)
    target_compile_definitions(aw_platform PUBLIC AW_PLATFORM_RPI=1)
    target_link_libraries(aw_platform PRIVATE PkgConfig::ALSA PkgConfig::GPIOD
                                              PkgConfig::USB PkgConfig::MTP)

    add_executable(aw-alsa-test rpi/alsa_test_main.cpp rpi/AlsaAudioOutput.cpp)
    target_include_directories(aw-alsa-test PRIVATE ${AW_INCLUDE_DIR})
    target_link_libraries(aw-alsa-test PRIVATE aw::core aw::hal aw::compiler_options
                                                   PkgConfig::ALSA Threads::Threads)

    add_executable(aw-gpio-test rpi/gpio_test_main.cpp rpi/GpioInput.cpp)
    target_include_directories(aw-gpio-test PRIVATE ${AW_INCLUDE_DIR})
    target_link_libraries(aw-gpio-test PRIVATE aw::core aw::hal aw::compiler_options
                                                   PkgConfig::GPIOD Threads::Threads)

endif()

add_library(aw::platform ALIAS aw_platform)
target_include_directories(aw_platform PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_platform
    PUBLIC aw::core aw::hal aw::platform_posix aw::platform_dev aw::compiler_options
           Threads::Threads)
