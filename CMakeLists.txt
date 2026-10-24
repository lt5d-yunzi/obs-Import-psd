# SPDX-License-Identifier: GPL-2.0-or-later
#
# Windows build for the obs-psd plugin (MSVC).
#
# The macOS build lives in build.sh.  This CMake project links against an
# *installed* OBS (default D:/64/obs-studio) using import libraries generated
# from its DLLs, plus a Qt 6 SDK matching the one OBS ships.
#
#   cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64
#   cmake --build build-win --config Release
#
# Paths can be overridden:
#   -DOBS_DIR=<portable obs dir>   (bin/64bit/obs.dll, obs-frontend-api.dll)
#   -DQT_ROOT=<Qt 6 msvc2022_64 prefix>
#   -DOBS_HEADERS=<obs-studio source root, tag 32.2.2>
cmake_minimum_required(VERSION 3.21)
project(obs-psd LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT DEFINED OBS_DIR OR OBS_DIR STREQUAL "")
  if(DEFINED ENV{OBS_DIR})
    set(OBS_DIR "$ENV{OBS_DIR}")
  else()
    set(OBS_DIR "D:/64/obs-studio")
  endif()
endif()
if(NOT DEFINED QT_ROOT OR QT_ROOT STREQUAL "")
  set(QT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qt6")
endif()
if(NOT DEFINED OBS_HEADERS OR OBS_HEADERS STREQUAL "")
  set(OBS_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/obs-src")
endif()

set(TP "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

if(NOT EXISTS "${OBS_DIR}/bin/64bit/obs.dll")
  message(FATAL_ERROR "obs.dll not found under OBS_DIR=${OBS_DIR}")
endif()
if(NOT EXISTS "${OBS_HEADERS}/libobs/obs.h")
  message(FATAL_ERROR "OBS headers not found under OBS_HEADERS=${OBS_HEADERS}")
endif()

list(APPEND CMAKE_PREFIX_PATH "${QT_ROOT}")
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

set(SOURCES
  src/plugin_main.cpp
  src/ui/psd_ui.cpp
  src/psd/psd_read.cpp
  src/psd/psd_write.cpp
  src/psd/psd_buf.cpp
  src/psd/rle.cpp
  src/psd/psd_text.cpp
  src/psd/psd_descriptor.cpp
  third_party/miniz/miniz.c
  third_party/miniz/miniz_tdef.c
  third_party/miniz/miniz_tinfl.c
  third_party/miniz/miniz_zip.c
  src/obs/source_render.cpp
  src/obs/scene_build.cpp
  src/obs/scene_export.cpp
  src/obs/psd_ignore.cpp
)

if(WIN32)
  list(APPEND SOURCES src/obs/text_render_qt.cpp)
else()
  list(APPEND SOURCES src/obs/text_render_macos.mm)
endif()

add_library(obs-psd SHARED ${SOURCES})

# Developer aid: import a PSD headlessly right after startup (OBS_PSD_SELFTEST).
option(OBS_PSD_SELFTEST "Build the developer self-test hook (import/export without dialogs)" OFF)
if(OBS_PSD_SELFTEST)
  target_sources(obs-psd PRIVATE src/obs/selftest.cpp)
  target_compile_definitions(obs-psd PRIVATE OBS_PSD_SELFTEST=1)
endif()

# libobs' public headers include <obsconfig.h>, which OBS generates from
# libobs/obsconfig.h.in during its own build.  Plugins only need the path
# defines, so generate a minimal one.
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")
file(WRITE "${GEN_DIR}/obsconfig.h"
"#pragma once
#define OBS_DATA_PATH \"data\"
#define OBS_PLUGIN_PATH \"obs-plugins/64bit\"
#define OBS_PLUGIN_DESTINATION \"obs-plugins/64bit\"
#define OBS_INSTALL_PREFIX \"\"
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
")

target_include_directories(obs-psd PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${TP}"
  "${TP}/miniz"
  "${OBS_HEADERS}/deps/w32-pthreads"
  "${GEN_DIR}"
  "${OBS_HEADERS}/libobs"
  "${OBS_HEADERS}/frontend/api"
)

# Import libraries generated from the installed OBS DLLs (obs.lib,
# obs-frontend-api.lib).  cmake/ make-import-libs.ps1 creates them.
set(OBS_IMPLIB_DIR "${TP}/obs-libs/x64")
if(NOT EXISTS "${OBS_IMPLIB_DIR}/obs.lib")
  message(FATAL_ERROR
    "obs.lib not found in ${OBS_IMPLIB_DIR}. Run:\n"
    "  powershell -ExecutionPolicy Bypass -File cmake/make-import-libs.ps1")
endif()

target_link_libraries(obs-psd PRIVATE
  "${OBS_IMPLIB_DIR}/obs.lib"
  "${OBS_IMPLIB_DIR}/obs-frontend-api.lib"
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
)

target_compile_definitions(obs-psd PRIVATE
  UNICODE _UNICODE NOMINMAX _CRT_SECURE_NO_WARNINGS
  WIN32_LEAN_AND_MEAN
)

if(MSVC)
  target_compile_options(obs-psd PRIVATE /utf-8 /EHsc /W3 /wd4996)
  # OBS itself links the static CRT, so a plugin that needs MSVCP140.dll would
  # not load on a machine without the VC++ redistributable.  Match OBS.
  set_property(TARGET obs-psd PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set_target_properties(obs-psd PROPERTIES
    PREFIX ""
    OUTPUT_NAME "obs-psd"
    SUFFIX ".dll"
  )
endif()

# OBS looks for the module's data under <obs>/data/obs-plugins/<module>/.
add_custom_command(TARGET obs-psd POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/data/obs-plugins/obs-psd/locale"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/en-US.ini"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/locale/zh-CN.ini"
    "${CMAKE_CURRENT_BINARY_DIR}/data/obs-plugins/obs-psd/locale"
)
