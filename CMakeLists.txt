cmake_minimum_required(VERSION 3.21)

# GPU Helper is an iOS-only Geode prototype. Objective-C++ is used only for
# the small Metal compute bridge; the rest of the mod remains C++.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 23)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_OSX_ARCHITECTURES "arm64")

project(GPUHelper VERSION 0.1.0 LANGUAGES C CXX OBJCXX)

if (NOT ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS))
    message(FATAL_ERROR "GPU Helper currently targets iOS only.")
endif()

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.mm"
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)

find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
find_library(OPENGLES_FRAMEWORK OpenGLES REQUIRED)
target_link_libraries(${PROJECT_NAME}
    ${METAL_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
    ${COREVIDEO_FRAMEWORK}
    ${OPENGLES_FRAMEWORK}
)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK. Define GEODE_SDK before configuring the project.")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})

# The Geode 2.2081 iOS codegen emits FMOD wrappers into GeneratedSource.cpp.
# Force-include the SDK's canonical FMOD declarations for that generated target.
if (CMAKE_SYSTEM_NAME STREQUAL "iOS" AND TARGET GeodeBindings)
    target_compile_options(GeodeBindings PRIVATE
        "-include"
        "Geode/fmod/fmod.hpp"
    )
endif()
