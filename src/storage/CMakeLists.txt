##
## aw_storage - project layout and safe-write helpers.
##
## This is the *policy* half of storage; the mechanism lives behind
## hal::StorageInterface.  That split is why these classes can be unit tested
## against an in-memory volume with no filesystem involved.
##
add_library(aw_storage STATIC
    AtomicFileWriter.cpp
    ProjectPaths.cpp)

add_library(aw::storage ALIAS aw_storage)

target_include_directories(aw_storage PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_storage PUBLIC aw::core aw::hal aw::compiler_options)
