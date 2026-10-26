##
## Module list, in dependency order (bottom of the stack first).
##
## The layering rule, enforced by what each target links:
##
##   core      <- nothing
##   hal       <- core
##   events    <- core, hal
##   storage   <- core, hal
##   audio     <- core, hal, events
##   mtp       <- core, hal, events
##   display   <- core, hal
##   project   <- core, hal, events, storage, audio
##   ui        <- everything above
##   platform  <- core, hal            (implements the interfaces)
##   app       <- everything           (composition root)
##
# Relative to this file, not CMAKE_SOURCE_DIR: the ESP-IDF firmware build adds
# this directory from its own project.
cmake_path(SET AW_INCLUDE_DIR NORMALIZE "${CMAKE_CURRENT_LIST_DIR}/../include")
set(AW_INCLUDE_DIR "${AW_INCLUDE_DIR}" CACHE INTERNAL "public headers")

add_subdirectory(core)
add_subdirectory(hal)
add_subdirectory(events)
add_subdirectory(storage)
add_subdirectory(audio)
add_subdirectory(mtp)
add_subdirectory(devices)
add_subdirectory(display)
add_subdirectory(project)
add_subdirectory(ui)
add_subdirectory(platform)
add_subdirectory(app)
