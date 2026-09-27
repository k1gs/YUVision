cmake_minimum_required(VERSION 3.24)
project(YUVision VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "YUVision is Windows-only")
endif()

add_executable(yuvision WIN32
  src/main.cpp
  src/video_capture.cpp
  src/video_capture.hpp
  src/renderer.cpp
  src/renderer.hpp
  src/audio_loop.cpp
  src/audio_loop.hpp
  src/common.hpp
)

set_target_properties(yuvision PROPERTIES OUTPUT_NAME "YUVision")

target_compile_features(yuvision PRIVATE cxx_std_20)
target_compile_definitions(yuvision PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(yuvision PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(yuvision PRIVATE
  mf mfplat mfreadwrite mfuuid
  d3d11 dxgi d3dcompiler
  d2d1 dwrite
  ole32 uuid avrt shcore
)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT yuvision)
