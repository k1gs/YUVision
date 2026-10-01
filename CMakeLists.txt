cmake_minimum_required(VERSION 3.24)
project(YUVision VERSION 0.1.1 LANGUAGES CXX)

option(YUVISION_ENABLE_MSI "Enable the WiX MSI package generator" OFF)

if(NOT WIN32)
  message(FATAL_ERROR "YUVision is Windows-only")
endif()

add_executable(yuvision WIN32
  src/resources/app.rc
  src/resources/resource.h
  src/app/main.cpp
  src/engine/audio/audio_loop.cpp
  src/engine/audio/audio_loop.hpp
  src/engine/capture/video_capture.cpp
  src/engine/capture/video_capture.hpp
  src/engine/common/common.hpp
  src/engine/render/renderer.cpp
  src/engine/render/renderer.hpp
)

set_target_properties(yuvision PROPERTIES
  OUTPUT_NAME "YUVision"
  # Keep release builds portable: users must not need to install the
  # Visual C++ Redistributable before YUVision can reach wWinMain.
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_compile_features(yuvision PRIVATE cxx_std_20)
target_include_directories(yuvision PRIVATE src)
target_compile_definitions(yuvision PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(yuvision PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(yuvision PRIVATE
  mf mfplat mfreadwrite mfuuid
  d3d11 dxgi d3dcompiler
  d2d1 dwrite
  ole32 uuid avrt shcore shell32
)

# Release archives must be generated from this install manifest instead of
# manually zipping a build-tree EXE.
install(TARGETS yuvision RUNTIME DESTINATION .)
install(FILES README.md LICENSE DESTINATION .)

set(CPACK_GENERATOR ZIP)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_PACKAGE_FILE_NAME "YUVision-v${PROJECT_VERSION}-windows-x64")
set(CPACK_PACKAGE_VENDOR "YUVision")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Low-latency YUY2 capture-card viewer")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "YUVision")
configure_file(LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")

if(YUVISION_ENABLE_MSI)
  list(APPEND CPACK_GENERATOR WIX)
  set(CPACK_WIX_VERSION 4)
  set(CPACK_WIX_ARCHITECTURE x64)
  set(CPACK_WIX_INSTALL_SCOPE perMachine)
  set(CPACK_WIX_UPGRADE_GUID "8495238D-8BA6-49BB-BBB9-FCD84F4F657C")
  set(CPACK_WIX_PRODUCT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/YUVision.ico")
  set(CPACK_WIX_PROGRAM_MENU_FOLDER "YUVision")
  set(CPACK_PACKAGE_EXECUTABLES "YUVision" "YUVision")
endif()

include(CPack)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT yuvision)
