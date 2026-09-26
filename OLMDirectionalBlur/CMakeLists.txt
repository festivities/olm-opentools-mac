# OLMDirectionalBlur — macOS (.plugin bundle) build.
# Usage (AE SDK 26.5 Mac at <sdk>):
#   cmake -S . -B build -DAE_SDK=<sdk>/Examples -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
#   cmake --build build --config Release
# Produces build/OLMDirectionalBlur.plugin/Contents/MacOS/OLMDirectionalBlur
# Install: copy the .plugin into
#   /Applications/Adobe After Effects 2026/Support Files/Plug-ins/OLM/

cmake_minimum_required(VERSION 3.28)
project(OLMDirectionalBlur LANGUAGES C CXX)

if(NOT DEFINED AE_SDK)
    message(FATAL_ERROR "Pass -DAE_SDK=<path to SDK Examples dir> (contains Headers/, Util/, Resources/)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0")

add_library(OLMDirectionalBlur MODULE OLMDirectionalBlur.cpp)

target_include_directories(OLMDirectionalBlur PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${AE_SDK}/Headers
    ${AE_SDK}/Util
    ${AE_SDK}/Resources
)

target_compile_definitions(OLMDirectionalBlur PRIVATE AE_OS_MAC=1)

# Optional OpenMP (mirrors Windows VCOMP140 usage). Builds fine without it.
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    target_link_libraries(OLMDirectionalBlur PRIVATE OpenMP::OpenMP_CXX)
endif()

set_target_properties(OLMDirectionalBlur PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION plugin
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Mac/OLMDirectionalBlur.plugin-Info.plist
    OUTPUT_NAME OLMDirectionalBlur
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)

# Rez-compile the PiPL resource (macOS SDK convention).
find_program(REZ Rez /usr/bin)
if(REZ)
    add_custom_command(TARGET OLMDirectionalBlur POST_BUILD
        COMMAND ${REZ}
            -d __MACH__ -d AE_OS_MAC=1
            -i ${AE_SDK}/Headers
            -i ${AE_SDK}/Resources
            -o $<TARGET_FILE:OLMDirectionalBlur>
            -t 'PiPL' -c 'eFKT'
            ${CMAKE_CURRENT_SOURCE_DIR}/OLMDirectionalBlurPiPL.r
        COMMENT "Compiling PiPL resource")
endif()
