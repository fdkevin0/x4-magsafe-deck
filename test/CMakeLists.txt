cmake_minimum_required(VERSION 3.16)
project(x4_magsafe_deck_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.18.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

add_executable(AncsDeckTest
  AncsDeckTest.cpp
  ../src/network/FirmwareBoardTag.cpp
  ../lib/Utf8/Utf8.cpp
)
target_compile_definitions(AncsDeckTest PRIVATE FREEINK_DEVICE_X4=1)
target_include_directories(AncsDeckTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/network
  ${CMAKE_CURRENT_SOURCE_DIR}/../lib
  ${CMAKE_CURRENT_SOURCE_DIR}/../lib/Memory
  ${CMAKE_CURRENT_SOURCE_DIR}/../lib/Utf8
)
target_link_libraries(AncsDeckTest PRIVATE GTest::gtest_main)
gtest_discover_tests(AncsDeckTest)
