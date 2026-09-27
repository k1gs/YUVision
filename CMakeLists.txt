cmake_minimum_required(VERSION 3.24)
project(YUVision VERSION 0.1.0 LANGUAGES CXX)

if(WIN32)
  add_executable(yuvision WIN32
    src/app.rc
    src/main.cpp
    src/video_capture.cpp
    src/video_capture.hpp
    src/renderer.cpp
    src/renderer.hpp
    src/audio_loop.cpp
    src/audio_loop.hpp
    src/common.hpp
    src/resource.h
  )
  set_target_properties(yuvision PROPERTIES OUTPUT_NAME "YUVision")
  target_compile_definitions(yuvision PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(yuvision PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_link_libraries(yuvision PRIVATE
    mf mfplat mfreadwrite mfuuid
    d3d11 dxgi d3dcompiler
    d2d1 dwrite
    ole32 uuid avrt shcore
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
  pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
  pkg_check_modules(OPENGL REQUIRED IMPORTED_TARGET gl)
  add_executable(yuvision src/linux_main.cpp)
  target_compile_options(yuvision PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(yuvision PRIVATE
    PkgConfig::SDL3 PkgConfig::ALSA PkgConfig::OPENGL pthread
  )
else()
  message(FATAL_ERROR "YUVision currently supports Windows and Linux")
endif()

target_compile_features(yuvision PRIVATE cxx_std_20)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT yuvision)
