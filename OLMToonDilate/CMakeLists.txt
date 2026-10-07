# OLMToonDilate — macOS (.plugin bundle) build.
# Usage (AE SDK 26.5 Mac at <sdk>):
#   cmake -S . -B build -DAE_SDK=<sdk>/Examples -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
#   cmake --build build --config Release
# Produces build/OLMToonDilate.plugin/Contents/MacOS/OLMToonDilate

cmake_minimum_required(VERSION 3.28)
project(OLMToonDilate LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds not allowed (they pollute the repo and confuse bundle output). Run: cmake -S . -B build ...")
endif()

if(NOT DEFINED AE_SDK)
    message(FATAL_ERROR "Pass -DAE_SDK=<path to SDK Examples dir> (contains Headers/, Util/, Resources/)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0")

add_library(OLMToonDilate MODULE
    OLMToonDilate.cpp
    ${AE_SDK}/Util/Smart_Utils.cpp
    ${AE_SDK}/Util/AEGP_SuiteHandler.cpp
    ${AE_SDK}/Util/MissingSuiteError.cpp
)

target_include_directories(OLMToonDilate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${AE_SDK}/Headers
    ${AE_SDK}/Headers/SP
    ${AE_SDK}/Util
    ${AE_SDK}/Resources
)

set_target_properties(OLMToonDilate PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION plugin
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Mac/OLMToonDilate.plugin-Info.plist
    OUTPUT_NAME OLMToonDilate
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)

execute_process(
    COMMAND xcrun -f Rez
    RESULT_VARIABLE _rez_rc
    OUTPUT_VARIABLE _rez_path
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(_rez_rc EQUAL 0 AND EXISTS "${_rez_path}")
    set(REZ_CMD xcrun Rez)
else()
    find_program(REZ_FALLBACK NAMES Rez PATHS /usr/bin)
    if(REZ_FALLBACK)
        set(REZ_CMD "${REZ_FALLBACK}")
    else()
        message(FATAL_ERROR
            "Rez not found (install full Xcode). Without an embedded PiPL resource "
            "After Effects will not load the plug-in at all.")
    endif()
endif()

find_program(CODESIGN_EXE NAMES codesign PATHS /usr/bin)
if(NOT CODESIGN_EXE)
    message(FATAL_ERROR "codesign not found")
endif()
find_program(XATTR_EXE NAMES xattr PATHS /usr/bin)
if(NOT XATTR_EXE)
    set(XATTR_EXE /usr/bin/xattr)
endif()

add_custom_command(TARGET OLMToonDilate POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_BUNDLE_DIR:OLMToonDilate>/Contents/Resources"
    COMMAND ${REZ_CMD}
        -o "$<TARGET_BUNDLE_DIR:OLMToonDilate>/Contents/Resources/OLMToonDilate.rsrc"
        -d SystemSevenOrLater=1
        -useDF
        -script Roman
        -arch arm64
        -arch x86_64
        -i "${AE_SDK}/Headers"
        -i "${AE_SDK}/Headers/SP"
        -i "${AE_SDK}/Resources"
        "${CMAKE_CURRENT_SOURCE_DIR}/OLMToonDilatePiPL.r"
    COMMAND "${XATTR_EXE}" -cr "$<TARGET_BUNDLE_DIR:OLMToonDilate>"
    COMMAND "${CODESIGN_EXE}" --force --sign - "$<TARGET_BUNDLE_DIR:OLMToonDilate>"
    COMMENT "Compiling PiPL to Contents/Resources and signing bundle (ad-hoc)")
