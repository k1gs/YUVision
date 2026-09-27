cmake_minimum_required(VERSION 3.24)
project(Viewer331YUY2Fix VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "331Viewer-YUY2Fix is Windows-only")
endif()

add_executable(viewer331 WIN32
  src/main.cpp
  src/video_capture.cpp
  src/video_capture.hpp
  src/renderer.cpp
  src/renderer.hpp
  src/audio_loop.cpp
  src/audio_loop.hpp
  src/common.hpp
)

set_target_properties(viewer331 PROPERTIES OUTPUT_NAME "331Viewer-YUY2Fix")

target_compile_features(viewer331 PRIVATE cxx_std_20)
target_compile_definitions(viewer331 PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(viewer331 PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(viewer331 PRIVATE
  mf mfplat mfreadwrite mfuuid
  d3d11 dxgi d3dcompiler
  d2d1 dwrite
  ole32 uuid avrt shcore
)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT viewer331)
