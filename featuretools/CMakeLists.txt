cmake_minimum_required(VERSION 3.25)

project(VillienFeatureTools VERSION 2.0.0 LANGUAGES CXX)

set(VILLIEN_FEATURE_CATALOG
    "${CMAKE_CURRENT_SOURCE_DIR}/feature_catalog.md")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(FeatureLabCore STATIC
  src/FeatureRegistry.cpp
  src/FeatureRegistry.h
  src/FeatureWorld.cpp
  src/FeatureWorld.h
)

target_compile_definitions(FeatureLabCore PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)
target_include_directories(FeatureLabCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_executable(FeatureTools WIN32
  src/main.cpp
  src/FeatureLabApp.cpp
  src/FeatureLabApp.h
  src/Dx12Renderer.cpp
  src/Dx12Renderer.h
)

target_compile_definitions(FeatureTools PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0A00
)

target_include_directories(FeatureTools PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(FeatureTools PRIVATE
  FeatureLabCore
  d3d12
  dxgi
  d3dcompiler
  dxguid
  comctl32
  shell32
  user32
  gdi32
)

if(MSVC)
  target_compile_options(FeatureLabCore PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_compile_options(FeatureTools PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

set_target_properties(FeatureTools PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_custom_command(TARGET FeatureTools POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "$<TARGET_FILE_DIR:FeatureTools>/shaders"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
          "$<TARGET_FILE_DIR:FeatureTools>/shaders"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${VILLIEN_FEATURE_CATALOG}"
          "$<TARGET_FILE_DIR:FeatureTools>/feature.md"
  COMMENT "Copying Feature Lab shaders and public feature catalog"
)

set_property(TARGET FeatureTools APPEND PROPERTY LINK_DEPENDS
  "${VILLIEN_FEATURE_CATALOG}"
)

enable_testing()

add_executable(FeatureLabTests
  tests/FeatureLabTests.cpp
)
target_compile_definitions(FeatureLabTests PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)
target_link_libraries(FeatureLabTests PRIVATE FeatureLabCore)
if(MSVC)
  target_compile_options(FeatureLabTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

add_test(
  NAME FeatureLabTests
  COMMAND FeatureLabTests "${VILLIEN_FEATURE_CATALOG}"
)
