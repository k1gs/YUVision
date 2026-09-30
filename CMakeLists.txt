cmake_minimum_required(VERSION 3.24)
project(YUVision VERSION 0.1.1 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "YUVision is Windows-only")
endif()

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

set_target_properties(yuvision PROPERTIES
  OUTPUT_NAME "YUVision"
  # Keep release builds portable: users must not need to install the
  # Visual C++ Redistributable before YUVision can reach wWinMain.
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_compile_features(yuvision PRIVATE cxx_std_20)
target_compile_definitions(yuvision PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(yuvision PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(yuvision PRIVATE
  mf mfplat mfreadwrite mfuuid
  d3d11 dxgi d3dcompiler
  d2d1 dwrite
  ole32 uuid avrt shcore
)

# Release archives must be generated from this install manifest instead of
# manually zipping a build-tree EXE.
install(TARGETS yuvision RUNTIME DESTINATION .)
install(FILES README.md DESTINATION .)

set(CPACK_GENERATOR ZIP)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_PACKAGE_FILE_NAME "YUVision-v${PROJECT_VERSION}-windows-x64")
include(CPack)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT yuvision)
