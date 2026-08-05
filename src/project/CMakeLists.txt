##
## aw_project - the data model and the only writer of project state.
##
## Links audio because it produces `audio::EngineScene` and because import
## writes WAV files.  The arrow never points back: aw_audio does not link this.
##
add_library(aw_project STATIC
    ExportService.cpp
    ImportService.cpp
    Project.cpp
    ProjectManager.cpp
    ProjectMetadata.cpp
    Timeline.cpp
    Track.cpp
    UndoStack.cpp)

add_library(aw::project ALIAS aw_project)

target_include_directories(aw_project PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_project
    PUBLIC aw::core aw::hal aw::events aw::storage aw::audio aw::compiler_options)
