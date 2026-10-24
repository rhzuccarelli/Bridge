##
## Bench: where an import's time goes. Times SD writes and reads through
## FatFsStorage, MTP reads from the TP-7 through EspMtpDevice, and the copy
## from one to the other, using the same backends as the bridge firmware.
##
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../bridge/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(storage-bench)
