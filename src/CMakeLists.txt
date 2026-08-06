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
set(AW_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include" CACHE INTERNAL "public headers")

add_subdirectory(core)
add_subdirectory(hal)
add_subdirectory(events)
add_subdirectory(storage)
add_subdirectory(audio)
add_subdirectory(mtp)
add_subdirectory(display)
add_subdirectory(project)
add_subdirectory(ui)
add_subdirectory(platform)
add_subdirectory(app)
