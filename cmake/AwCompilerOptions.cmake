##
## Shared compiler settings, exposed as an INTERFACE target every module links.
##
## Keeping the flags in one target (rather than in CMAKE_CXX_FLAGS) means a
## future ESP-IDF component build can pick and choose without inheriting a
## global string it cannot override.
##
add_library(aw_compiler_options INTERFACE)
add_library(aw::compiler_options ALIAS aw_compiler_options)

option(AW_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aw_compiler_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Woverloaded-virtual
        -Wnull-dereference
        -Wdouble-promotion   # catches accidental float->double in the audio path
        -Wformat=2)
    if(AW_WARNINGS_AS_ERRORS)
        target_compile_options(aw_compiler_options INTERFACE -Werror)
    endif()
endif()
