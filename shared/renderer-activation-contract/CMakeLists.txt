cmake_minimum_required(VERSION 3.20)
project(mactype_renderer_activation_contract LANGUAGES C CXX)

enable_testing()

add_executable(renderer-activation-contract-tests contract_tests.cpp)
target_compile_features(renderer-activation-contract-tests PRIVATE cxx_std_17)
target_include_directories(
  renderer-activation-contract-tests
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if(MSVC)
  target_compile_options(renderer-activation-contract-tests PRIVATE /W4 /WX /permissive-)
endif()

add_test(
  NAME renderer-activation-contract
  COMMAND renderer-activation-contract-tests
)

add_executable(renderer-activation-c-abi-tests c_abi_tests.c)
target_include_directories(
  renderer-activation-c-abi-tests
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if(MSVC)
  target_compile_options(renderer-activation-c-abi-tests PRIVATE /W4 /WX)
endif()

add_test(
  NAME renderer-activation-c-abi
  COMMAND renderer-activation-c-abi-tests
)
