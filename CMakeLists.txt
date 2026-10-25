cmake_minimum_required(VERSION 3.16)
project(rv32sim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion)
endif()

add_library(rv32core src/decoder.cpp src/cpu.cpp src/elf_loader.cpp)
target_include_directories(rv32core PUBLIC include)

add_executable(rv32sim src/main.cpp)
target_link_libraries(rv32sim PRIVATE rv32core)

option(RV32SIM_TESTS "Build unit tests (uses system GoogleTest, else fetches it)" ON)
if(RV32SIM_TESTS)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  enable_testing()
  add_executable(unit_tests
    tests/unit/test_decoder.cpp
    tests/unit/test_execute.cpp
    tests/unit/test_memory.cpp)
  target_link_libraries(unit_tests PRIVATE rv32core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(unit_tests)
endif()
