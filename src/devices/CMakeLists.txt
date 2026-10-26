##
## aw_devices - settings of external devices that Bridge controls over USB.
##
add_library(aw_devices STATIC ApogeeDuet.cpp)

add_library(aw::devices ALIAS aw_devices)

target_include_directories(aw_devices PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_devices PUBLIC aw::core aw::hal aw::compiler_options)
