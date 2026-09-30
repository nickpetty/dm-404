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
    Source/DawBridge.cpp
    Source/WinMidi.cpp)

target_compile_definitions(Doom404 PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Doom404,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Doom404,JUCE_VERSION>"
    JUCE_DISPLAY_SPLASH_SCREEN=0)

# Windows MIDI Services: the app's own "Doom-404" MIDI port (WinMidi.cpp).
# Only the SDK's metadata is used, to generate C++/WinRT headers; its
# runtime is not shipped (Windows provides it, or the user adds it).
if(WIN32)
    option(DOOM404_WINMIDI "The Doom-404 MIDI port through Windows MIDI Services" ON)
endif()
if(WIN32 AND DOOM404_WINMIDI)
    set(MIDI2_VERSION "0.99.83-devpreview.9")
    set(MIDI2_RELEASE "inbox-dev-preview-9")
    set(MIDI2_DIR "${CMAKE_BINARY_DIR}/midi2")
    set(MIDI2_NUPKG "${MIDI2_DIR}/Windows.Devices.Midi2.${MIDI2_VERSION}.nupkg")
    set(MIDI2_WINMD "${MIDI2_DIR}/pkg/ref/native/Windows.Devices.Midi2.winmd")
    if(NOT EXISTS "${MIDI2_WINMD}")
        if(NOT EXISTS "${MIDI2_NUPKG}")
            message(STATUS "Fetching the Windows MIDI Services SDK metadata (${MIDI2_VERSION})")
            file(DOWNLOAD "https://github.com/microsoft/MIDI/releases/download/${MIDI2_RELEASE}/Windows.Devices.Midi2.${MIDI2_VERSION}.nupkg"
                 "${MIDI2_NUPKG}" STATUS dl)
        endif()
        file(ARCHIVE_EXTRACT INPUT "${MIDI2_NUPKG}" DESTINATION "${MIDI2_DIR}/pkg" PATTERNS "ref/native/*")
    endif()
    set(SDK_VER "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
    find_program(CPPWINRT cppwinrt
        HINTS "C:/Program Files (x86)/Windows Kits/10/bin/${SDK_VER}/x64" "$ENV{WindowsSdkBinPath}/x64")
    if(EXISTS "${MIDI2_WINMD}" AND CPPWINRT)
        if(NOT EXISTS "${MIDI2_DIR}/gen/winrt/Windows.Devices.Midi2.h")
            execute_process(COMMAND "${CPPWINRT}" -input "${MIDI2_WINMD}" -input "${SDK_VER}" -output "${MIDI2_DIR}/gen"
                            RESULT_VARIABLE gen_result)
        endif()
        target_include_directories(Doom404 BEFORE PRIVATE "${MIDI2_DIR}/gen")
        target_compile_definitions(Doom404 PRIVATE DOOM404_WINMIDI=1)
        target_link_libraries(Doom404 PRIVATE windowsapp)
        set_source_files_properties(Source/WinMidi.cpp PROPERTIES COMPILE_OPTIONS "/bigobj")
    else()
        message(WARNING "Windows MIDI Services SDK or cppwinrt not found: no Doom-404 MIDI port on Windows")
    endif()
endif()

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
