##
## Unit tests.
##
## Every test here links only portable modules - no ALSA, no GPIO, no display
## driver, no platform backend at all.  That is the architectural claim made in
## Phase 1, and this target is what proves it: if a business-logic module ever
## grows a hardware dependency, this stops linking.
##
add_executable(aw_tests
    framework/Test.cpp
    support/MemoryStorage.cpp
    audio/AudioTests.cpp
    core/CoreTests.cpp
    display/DisplayTests.cpp
    events/EventTests.cpp
    mtp/MtpTests.cpp
    project/ProjectTests.cpp
    storage/StorageTests.cpp
    ui/UiTests.cpp)

target_include_directories(aw_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(aw_tests PRIVATE
    aw::core
    aw::hal
    aw::events
    aw::storage
    aw::audio
    aw::project
    aw::mtp
    aw::display
    aw::ui
    aw::compiler_options)

find_package(Threads REQUIRED)
target_link_libraries(aw_tests PRIVATE Threads::Threads)

add_test(NAME unit COMMAND aw_tests)

##
## Platform backend tests.
##
## A separate executable on purpose.  `aw_tests` above links no platform code,
## and that is the architectural claim this project makes; folding these in
## would destroy the only mechanical check of it.  Only the pure-logic half of
## each backend is covered here - key mapping, panel geometry, source merging -
## so the suite needs no framebuffer, touchscreen or terminal to run.
##
add_executable(aw_platform_tests
    framework/Test.cpp
    platform/PlatformTests.cpp)

target_include_directories(aw_platform_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aw_platform_tests PRIVATE
    aw::core
    aw::hal
    aw::platform_dev
    aw::compiler_options
    Threads::Threads)

add_test(NAME platform COMMAND aw_platform_tests)
