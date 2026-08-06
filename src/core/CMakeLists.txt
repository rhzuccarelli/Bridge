##
## aw_core - foundational vocabulary types.
##
## Depends on nothing but the C++ standard library.  Anything that would make
## this module depend on another module belongs in that other module instead.
##
add_library(aw_core STATIC
    Controls.cpp
    Error.cpp
    Logger.cpp)

add_library(aw::core ALIAS aw_core)

target_include_directories(aw_core PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_core PUBLIC aw::compiler_options)
