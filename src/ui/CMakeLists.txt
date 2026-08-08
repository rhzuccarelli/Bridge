##
## aw_ui - screens, navigation and input routing.
##
## The top of the portable stack.  It links everything below it and nothing
## links it back, so a change to a screen can never ripple into the audio path.
## It draws through aw_display and reads the model through const references; it
## owns no model state of its own.
##
add_library(aw_ui STATIC
    Navigator.cpp
    Screen.cpp
    UiController.cpp
    screens/CopyChannelScreen.cpp
    screens/ExportScreen.cpp
    screens/HomeScreen.cpp
    screens/ImportScreen.cpp
    screens/PlaybackScreen.cpp
    screens/ProjectBrowserScreen.cpp
    screens/SettingsScreen.cpp
    screens/TrackViewScreen.cpp)

add_library(aw::ui ALIAS aw_ui)

target_include_directories(aw_ui PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_ui
    PUBLIC aw::core aw::hal aw::events aw::display aw::project aw::audio aw::mtp
           aw::compiler_options)
