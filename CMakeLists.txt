cmake_minimum_required(VERSION 3.10)
project(baktsiu)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
set(VERSION "0.2.1" CACHE STRING " " FORCE)

option(USE_NVTX "Use NVidia Tool Extensions" OFF)
option(USE_OPENEXR "Use OpenEXR" OFF)
option(EMBED_SHADERS "Embed shaders into binary" ON)
# Windows: ON by default (requires libmpv: MPV_ROOT or MPV_INCLUDE_DIR + MPV_LIBRARY). Set OFF to build without video.
if(WIN32)
    option(USE_VIDEO "Video playback via libmpv (OpenGL render)" ON)
else()
    option(USE_VIDEO "Video playback via libmpv (OpenGL render)" OFF)
endif()

if(USE_OPENEXR AND NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ext/openexr/OpenEXR")
    message(FATAL_ERROR "Missing dependent thrid-party submodules (like OpenEXR)!"
        "Please execute following command to recover:"
        "\"git submodule update --init --recursive\"")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release"
        "MinSizeRel" "RelWithDebInfo")
endif()

if(MSVC)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON) # Enable subfolder filters in VC project
endif()

if(WIN32)
    set(EXTRA_SRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.rc")
elseif(APPLE)
    set(EXTRA_SRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.icns")
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_EXTENSIONS OFF)

# CMake 4+ rejects cmake_minimum_required(VERSION <3.5) in bundled deps (GLFW, etc.).
# See: https://cmake.org/cmake/help/latest/variable/CMAKE_POLICY_VERSION_MINIMUM.html
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "Policy floor for bundled CMake deps under CMake 4+")
endif()

add_subdirectory(ext)
add_subdirectory(src)