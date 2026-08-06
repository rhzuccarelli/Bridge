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
    desktop/DevBackends.cpp
    desktop/KeyboardInput.cpp
    desktop/TerminalDisplay.cpp
    desktop/VirtualPanel.cpp
    linux/EvdevTouch.cpp
    linux/FramebufferDisplay.cpp)
add_library(aw::platform_dev ALIAS aw_platform_dev)
target_include_directories(aw_platform_dev PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_platform_dev
    PUBLIC aw::core aw::hal aw::compiler_options Threads::Threads)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "aw_platform_dev needs Linux (framebuffer + evdev). "
        "Build with AW_PLATFORM=simulator and AW_DISPLAY=memory elsewhere.")
endif()

if(AW_PLATFORM STREQUAL "simulator")

    add_library(aw_platform STATIC simulator/SimulatorPlatform.cpp)
    target_compile_definitions(aw_platform PUBLIC AW_PLATFORM_SIMULATOR=1)

elseif(AW_PLATFORM STREQUAL "rpi")

    add_library(aw_platform STATIC
        rpi/AlsaAudioOutput.cpp
        rpi/GpioInput.cpp
        rpi/LibMtpDevice.cpp
        rpi/LibUsbHost.cpp
        rpi/RpiPlatform.cpp
        rpi/SpiDisplay.cpp)
    target_compile_definitions(aw_platform PUBLIC AW_PLATFORM_RPI=1)

    # Phase 5/6/7/8 add the real dependencies here.  They are looked up but not
    # required yet, so the Pi target builds today with only the stubs.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ALSA QUIET alsa)
        pkg_check_modules(GPIOD QUIET libgpiod)
        pkg_check_modules(USB QUIET libusb-1.0)
        pkg_check_modules(MTP QUIET libmtp)
        foreach(dep ALSA GPIOD USB MTP)
            if(NOT ${dep}_FOUND)
                message(STATUS "  rpi backend: ${dep} not found (stubs remain in use)")
            endif()
        endforeach()
    endif()

endif()

add_library(aw::platform ALIAS aw_platform)
target_include_directories(aw_platform PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_platform
    PUBLIC aw::core aw::hal aw::platform_posix aw::platform_dev aw::compiler_options
           Threads::Threads)
