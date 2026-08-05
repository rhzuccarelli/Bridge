##
## aw_display - software rendering into an RGB565 framebuffer.
##
## Knows nothing about screens, projects or events; it draws rectangles, text
## and waveforms.  That is what lets a test render a full UI frame and assert on
## pixels with no hardware attached.
##
add_library(aw_display STATIC
    DisplayFlusher.cpp
    Font.cpp
    Framebuffer.cpp
    Painter.cpp)

add_library(aw::display ALIAS aw_display)

target_include_directories(aw_display PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_display PUBLIC aw::core aw::hal aw::compiler_options)
