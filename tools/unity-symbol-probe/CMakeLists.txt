cmake_minimum_required(VERSION 3.20)

project(mactype_unity_symbol_probe LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "unity-symbol-probe requires the Microsoft DIA SDK")
endif()

find_path(DIA_SDK_INCLUDE_DIR dia2.h
  HINTS
    "$ENV{VSINSTALLDIR}/DIA SDK/include"
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_dia_library_suffix amd64)
else()
  set(_dia_library_suffix "")
endif()

find_library(DIA_GUIDS_LIBRARY diaguids
  HINTS
    "$ENV{VSINSTALLDIR}/DIA SDK/lib/${_dia_library_suffix}"
    "$ENV{VSINSTALLDIR}/DIA SDK/lib"
)

if(NOT DIA_SDK_INCLUDE_DIR OR NOT DIA_GUIDS_LIBRARY)
  message(FATAL_ERROR "The Visual Studio DIA SDK was not found")
endif()

add_executable(mactype-unity-symbol-probe main.cpp)
target_compile_features(mactype-unity-symbol-probe PRIVATE cxx_std_17)
target_include_directories(mactype-unity-symbol-probe PRIVATE "${DIA_SDK_INCLUDE_DIR}")
target_link_libraries(mactype-unity-symbol-probe PRIVATE
  "${DIA_GUIDS_LIBRARY}"
  ole32
  oleaut32
)
target_compile_definitions(mactype-unity-symbol-probe PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)
