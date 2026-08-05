##
## aw_audio - WAV I/O and the realtime engine.
##
## Links core, hal and events.  Notably it does NOT link aw_project: the engine
## is driven by the plain-value `EngineScene`, so the whole audio path can be
## tested without a project on disk.
##
add_library(aw_audio STATIC
    AudioBuffer.cpp
    AudioEngine.cpp
    Mixer.cpp
    SampleConverter.cpp
    TrackPlayer.cpp
    Transport.cpp
    WavReader.cpp
    WavWriter.cpp)

add_library(aw::audio ALIAS aw_audio)

target_include_directories(aw_audio PUBLIC ${AW_INCLUDE_DIR})
target_link_libraries(aw_audio PUBLIC aw::core aw::hal aw::events aw::compiler_options)
