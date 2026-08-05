##
## Platform backends.
##
## Exactly one backend library is built, selected by AW_PLATFORM, and it is the
## only library in the project allowed to include a vendor header.  The alias
## `aw::platform` lets the application link "the platform" without naming which.
##
add_library(aw_platform_posix STATIC posix/PosixStorage.cpp)
add_library(aw::platform_posix ALIAS aw_platform_posix)
target_include_directories(aw_platform_posix PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_platform_posix PUBLIC aw::core aw::hal aw::compiler_options)

if(AW_PLATFORM STREQUAL "simulator")

    add_library(aw_platform STATIC simulator/SimulatorPlatform.cpp)
    target_link_libraries(aw_platform PUBLIC aw::platform_posix)
    target_compile_definitions(aw_platform PUBLIC AW_PLATFORM_SIMULATOR=1)
    find_package(Threads REQUIRED)
    target_link_libraries(aw_platform PUBLIC Threads::Threads)

elseif(AW_PLATFORM STREQUAL "rpi")

    add_library(aw_platform STATIC
        rpi/AlsaAudioOutput.cpp
        rpi/GpioInput.cpp
        rpi/LibMtpDevice.cpp
        rpi/LibUsbHost.cpp
        rpi/RpiPlatform.cpp
        rpi/SpiDisplay.cpp)
    target_link_libraries(aw_platform PUBLIC aw::platform_posix)
    target_compile_definitions(aw_platform PUBLIC AW_PLATFORM_RPI=1)
    find_package(Threads REQUIRED)
    target_link_libraries(aw_platform PUBLIC Threads::Threads)

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
target_link_libraries(aw_platform PUBLIC aw::core aw::hal aw::compiler_options)
