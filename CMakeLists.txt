cmake_minimum_required(VERSION 3.22)

project(ExtremeStretch VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Installs the built VST3 into ~/.vst3 so a host can find it. Turn OFF to keep
# every artefact inside the build tree.
option(ES_COPY_PLUGIN "Copy the built VST3 into the user plugin folder" ON)

find_package(JUCE CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Core DSP + engine. Deliberately free of any GUI dependency so the self-test
# console app can link exactly the same objects the plugin runs.
# ---------------------------------------------------------------------------
set(ES_CORE_SOURCES
    Source/PaulStretch.cpp
    Source/StretchEngine.cpp
    Source/Duration.cpp)

# ---------------------------------------------------------------------------
# Plugin / standalone app
# ---------------------------------------------------------------------------
juce_add_plugin(ExtremeStretch
    COMPANY_NAME            "SeriousSHIT"
    COMPANY_WEBSITE         "https://github.com/willbearfruits"
    PLUGIN_MANUFACTURER_CODE Sshi
    PLUGIN_CODE             Xstr
    FORMATS                 VST3 Standalone
    PRODUCT_NAME            "Extreme Stretch"
    IS_SYNTH                FALSE
    NEEDS_MIDI_INPUT        FALSE
    NEEDS_MIDI_OUTPUT       FALSE
    IS_MIDI_EFFECT          FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${ES_COPY_PLUGIN})

target_sources(ExtremeStretch PRIVATE
    ${ES_CORE_SOURCES}
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Ui.cpp)

target_compile_definitions(ExtremeStretch PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

target_link_libraries(ExtremeStretch PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_audio_devices
    juce::juce_dsp
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Self-test. `ctest` or run the binary directly; must print ALL PASS.
# ---------------------------------------------------------------------------
juce_add_console_app(ExtremeStretchSelfTest
    PRODUCT_NAME "extremestretch-selftest")

target_sources(ExtremeStretchSelfTest PRIVATE
    ${ES_CORE_SOURCES}
    Tests/SelfTest.cpp)

target_compile_definitions(ExtremeStretchSelfTest PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(ExtremeStretchSelfTest PRIVATE
    juce::juce_audio_formats
    juce::juce_audio_basics
    juce::juce_dsp
    juce::juce_core
    juce::juce_events
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

set_target_properties(ExtremeStretchSelfTest PROPERTIES OUTPUT_NAME "extremestretch-selftest")

# ---------------------------------------------------------------------------
# Headless renderer. paulstretch has always been an offline tool; this is the
# scriptable half of the same engine.
# ---------------------------------------------------------------------------
juce_add_console_app(ExtremeStretchRender
    PRODUCT_NAME "extremestretch-render")

target_sources(ExtremeStretchRender PRIVATE
    ${ES_CORE_SOURCES}
    Tools/Render.cpp)

target_compile_definitions(ExtremeStretchRender PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(ExtremeStretchRender PRIVATE
    juce::juce_audio_formats
    juce::juce_audio_basics
    juce::juce_dsp
    juce::juce_core
    juce::juce_events
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

set_target_properties(ExtremeStretchRender PROPERTIES OUTPUT_NAME "extremestretch-render")

enable_testing()
add_test(NAME selftest COMMAND ExtremeStretchSelfTest)
