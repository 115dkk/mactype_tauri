cmake_minimum_required(VERSION 3.24)
project(mactype_preview32 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "mactype-preview32 is a Windows-only program")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "mactype-preview32 must be configured with -A Win32")
endif()

add_executable(mactype-preview32
  main.cpp
  protocol.cpp
  include/protocol.h
)
target_compile_features(mactype-preview32 PRIVATE cxx_std_20)
target_include_directories(mactype-preview32 PRIVATE include)
target_compile_definitions(mactype-preview32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
  target_compile_options(mactype-preview32 PRIVATE /W4 /WX /permissive- /utf-8 /guard:cf)
  # HIGHENTROPYVA is only valid for 64-bit images. This helper is deliberately
  # Win32 so it can host the existing 32-bit MacType preview path.
  target_link_options(mactype-preview32 PRIVATE /DYNAMICBASE /NXCOMPAT)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(protocol-tests tests/protocol_tests.cpp protocol.cpp include/protocol.h)
  target_compile_features(protocol-tests PRIVATE cxx_std_20)
  target_include_directories(protocol-tests PRIVATE include)
  if(MSVC)
    target_compile_options(protocol-tests PRIVATE /W4 /WX /permissive- /utf-8)
  endif()
  add_test(NAME protocol-tests COMMAND protocol-tests)
endif()
