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

target_sources(Doom404 PRIVATE
    Source/Main.cpp
    Source/EmulatorLink.cpp
    Source/PanelComponent.cpp
    Source/DebugPanel.cpp)

target_compile_definitions(Doom404 PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Doom404,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Doom404,JUCE_VERSION>"
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Doom404 PRIVATE
    juce::juce_gui_extra
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
