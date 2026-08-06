##
## aw_hal - hardware abstraction interfaces.
##
## Header only, and it must stay that way.  This module contains no behaviour,
## only the contracts that platform backends implement and portable code calls.
## If a .cpp ever appears here, some logic has leaked into the abstraction.
##
add_library(aw_hal INTERFACE)
add_library(aw::hal ALIAS aw_hal)

target_include_directories(aw_hal INTERFACE ${AW_INCLUDE_DIR})
target_link_libraries(aw_hal INTERFACE aw::core aw::compiler_options)
