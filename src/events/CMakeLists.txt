##
## aw_events - the wiring between producers and the application.
##
## Links core and hal only.  It must never learn what a project or a screen is;
## the arrow points the other way.
##
add_library(aw_events STATIC
    EventBus.cpp
    InputEventSource.cpp)

add_library(aw::events ALIAS aw_events)

target_include_directories(aw_events PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_events PUBLIC aw::core aw::hal aw::compiler_options)
