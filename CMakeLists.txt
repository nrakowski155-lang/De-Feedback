cmake_minimum_required(VERSION 3.22)

project(DeFeedback VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (APPLE)
    set(DEFB_PLUGIN_FORMATS AU VST3 Standalone)
    if (NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
    endif()
else()
    # Audio Units only exist on macOS; on other systems we still build VST3
    # and the standalone app so the code can be compiled and checked.
    set(DEFB_PLUGIN_FORMATS VST3 Standalone)
endif()

option(DEFB_BUILD_TESTS "Build the offline DSP test program" ON)
option(DEFB_COPY_PLUGIN_AFTER_BUILD "Copy the built plugins into the user's plugin folders" ON)

# JUCE can either be fetched or taken from a local checkout:
#   cmake -B build -DJUCE_PATH=/path/to/JUCE
if (DEFINED JUCE_PATH)
    add_subdirectory(${JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(DeFeedback
    COMPANY_NAME            "De-Feedback Audio"
    BUNDLE_ID               "com.defeedback.plugin"
    IS_SYNTH                FALSE
    NEEDS_MIDI_INPUT        FALSE
    NEEDS_MIDI_OUTPUT       FALSE
    IS_MIDI_EFFECT          FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${DEFB_COPY_PLUGIN_AFTER_BUILD}
    PLUGIN_MANUFACTURER_CODE Dfbk
    PLUGIN_CODE             Dfb1
    FORMATS                 ${DEFB_PLUGIN_FORMATS}
    PRODUCT_NAME            "De-Feedback"
    DESCRIPTION             "Adaptive feedback suppressor"
    VST3_CATEGORIES         Fx
    AU_MAIN_TYPE            kAudioUnitType_Effect)

target_sources(DeFeedback PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/dsp/DeFeedbackEngine.cpp)

target_include_directories(DeFeedback PRIVATE Source)

target_compile_definitions(DeFeedback PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

target_link_libraries(DeFeedback
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Framework-free DSP tests (run on any platform with a C++17 compiler)
if (DEFB_BUILD_TESTS)
    add_executable(defb_dsp_tests
        tests/dsp_test.cpp
        Source/dsp/DeFeedbackEngine.cpp)
    target_include_directories(defb_dsp_tests PRIVATE Source/dsp)
    set_target_properties(defb_dsp_tests PROPERTIES CXX_STANDARD 17)
endif()
