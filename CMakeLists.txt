cmake_minimum_required(VERSION 3.22)
project(NoteForge VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # no VC++ runtime needed on users' PCs

# JUCE 8.0.9: use a local checkout if given (-DJUCE_DIR=...), otherwise download it automatically.
set(JUCE_DIR "" CACHE PATH "Optional local JUCE checkout (downloaded automatically when empty)")
if(JUCE_DIR AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory(${JUCE_DIR} juce EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# Steinway Model B samples (University of Iowa MIS, free for any use), prepared by Tools/SamplePrep.cpp
file(GLOB NF_PIANO_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Resources/piano/*.flac")
juce_add_binary_data(NoteForgePianoData HEADER_NAME PianoData.h NAMESPACE PianoData
    SOURCES ${NF_PIANO_FILES} Resources/piano/manifest.txt)
set_target_properties(NoteForgePianoData PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(NOTEFORGE_COPY_PLUGIN OFF CACHE BOOL "Copy the VST3 into the system VST3 folder after building")

# ---- DSP core (no JUCE GUI), shared by the plugin and the test harness ----
set(NF_DSP_SOURCES
    Source/dsp/FFT.cpp
    Source/dsp/PitchDetector.cpp
    Source/dsp/Analysis.cpp
    Source/dsp/Renderer.cpp
    Source/dsp/LiveTuner.cpp
    Source/dsp/Spectrogram.cpp
    Source/dsp/PianoSynth.cpp
    Source/dsp/Align.cpp
    Source/model/Scale.cpp
    Source/model/NoteModel.cpp)

option(NF_TESTS_ONLY "Only build the DSP test harness" OFF)
if(NOT NF_TESTS_ONLY)
juce_add_plugin(NoteForge
    COMPANY_NAME "NoteForge"
    PLUGIN_MANUFACTURER_CODE Nfge
    PLUGIN_CODE NFrg
    FORMATS VST3
    PRODUCT_NAME "NoteForge"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES Fx Pitch Shift
    COPY_PLUGIN_AFTER_BUILD ${NOTEFORGE_COPY_PLUGIN})

target_sources(NoteForge PRIVATE
    ${NF_DSP_SOURCES}
    Source/Engine.cpp
    Source/PluginProcessor.cpp
    Source/ui/PluginEditor.cpp
    Source/ui/NoteEditor.cpp
    Source/ui/Inspector.cpp
    Source/ui/NotePicker.cpp
    Source/ui/FlBridge.cpp
    Source/ui/LiveView.cpp)

target_include_directories(NoteForge PRIVATE Source)

target_compile_definitions(NoteForge PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_USE_MP3AUDIOFORMAT=1)

target_link_libraries(NoteForge
    PRIVATE juce::juce_audio_utils juce::juce_audio_formats NoteForgePianoData
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)
endif()

# ---- Offline test harness: synthetic vocal -> analysis -> edits -> render -> verify ----
juce_add_console_app(NoteForgeTests PRODUCT_NAME "NoteForgeTests")
target_sources(NoteForgeTests PRIVATE
    ${NF_DSP_SOURCES}
    Source/Engine.cpp
    Tests/TestMain.cpp)
if(NOT NF_TESTS_ONLY)
    target_sources(NoteForgeTests PRIVATE
        Source/PluginProcessor.cpp
        Source/ui/PluginEditor.cpp
        Source/ui/NoteEditor.cpp
        Source/ui/Inspector.cpp
        Source/ui/NotePicker.cpp
        Source/ui/FlBridge.cpp
        Source/ui/LiveView.cpp)
    target_link_libraries(NoteForgeTests PRIVATE juce::juce_audio_utils juce::juce_audio_processors)
    target_compile_definitions(NoteForgeTests PRIVATE NOTEFORGE_UI_SNAPSHOT=1 JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_PLUGINHOST_VST3=1 JucePlugin_Name="NoteForge")
endif()
target_include_directories(NoteForgeTests PRIVATE Source)
target_compile_definitions(NoteForgeTests PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 NOTEFORGE_TESTS=1)
target_link_libraries(NoteForgeTests
    PRIVATE juce::juce_gui_basics juce::juce_audio_formats juce::juce_audio_basics NoteForgePianoData
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# ---- Tool: builds Resources/piano (embedded Steinway samples) from the raw Iowa MIS recordings ----
juce_add_console_app(NoteForgeSamplePrep PRODUCT_NAME "NoteForgeSamplePrep")
target_sources(NoteForgeSamplePrep PRIVATE ${NF_DSP_SOURCES} Tools/SamplePrep.cpp)
target_include_directories(NoteForgeSamplePrep PRIVATE Source)
target_compile_definitions(NoteForgeSamplePrep PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(NoteForgeSamplePrep
    PRIVATE juce::juce_audio_formats juce::juce_gui_basics NoteForgePianoData
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
