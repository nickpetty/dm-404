cmake_minimum_required(VERSION 3.25)
project(Doom404 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE: a local checkout if one is given (-DJUCE_DIR=...), else fetched.
include(FetchContent)
set(JUCE_DIR "" CACHE PATH "Existing JUCE 9 source tree (optional)")
if(JUCE_DIR)
    add_subdirectory(${JUCE_DIR} juce)
else()
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        9.0.2
        GIT_SHALLOW    ON)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_gui_app(Doom404
    PRODUCT_NAME "Doom-404"
    COMPANY_NAME "doom-404")

juce_generate_juce_header(Doom404)

# The panel's bindings, built in (a development run reads the repo's copy).
juce_add_binary_data(Doom404Data SOURCES panel.json)

# FatFs (ChaN, BSD-style licence): the drive images.
add_library(fatfs STATIC
    ThirdParty/fatfs/ff.c
    ThirdParty/fatfs/ffunicode.c
    ThirdParty/fatfs/ffsystem.c)

target_sources(Doom404 PRIVATE
    Source/Main.cpp
    Source/EmulatorLink.cpp
    Source/PanelComponent.cpp
    Source/DebugPanel.cpp
    Source/FatImage.cpp
    Source/Storage.cpp
    Source/SdCardWindow.cpp
    Source/DawBridge.cpp)

target_compile_definitions(Doom404 PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Doom404,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Doom404,JUCE_VERSION>"
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Doom404 PRIVATE
    Doom404Data
    fatfs
    juce::juce_cryptography
    juce::juce_gui_extra
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# Doom-404 Link: the DAW plugin that carries the running app's unit (audio
# both ways) into a DAW track. Plugin/LinkProcessor.h has the details.
if(APPLE)
    set(DOOM404_PLUGIN_FORMATS VST3 AU)
else()
    set(DOOM404_PLUGIN_FORMATS VST3)
endif()

juce_add_plugin(Doom404Link
    PRODUCT_NAME "Doom-404 Link"
    COMPANY_NAME "doom-404"
    PLUGIN_MANUFACTURER_CODE D404
    PLUGIN_CODE D4Lk
    FORMATS ${DOOM404_PLUGIN_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx Sampler
    COPY_PLUGIN_AFTER_BUILD FALSE)

juce_generate_juce_header(Doom404Link)

target_sources(Doom404Link PRIVATE
    Plugin/LinkProcessor.cpp)

target_compile_definitions(Doom404Link PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Doom404Link PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# A stand-in DAW for testing the plugin: loads the VST3 and runs it in real
# time against the running app (Tests/LinkHostTest.cpp).
juce_add_console_app(LinkHostTest PRODUCT_NAME "LinkHostTest")
juce_generate_juce_header(LinkHostTest)
target_sources(LinkHostTest PRIVATE Tests/LinkHostTest.cpp)
target_compile_definitions(LinkHostTest PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(LinkHostTest PRIVATE
    juce::juce_audio_processors
    juce::juce_recommended_config_flags)
