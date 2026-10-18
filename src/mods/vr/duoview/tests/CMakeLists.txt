# DuoView host logic tests — pure C++ (no engine, no GPU).
# Build: cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake_minimum_required(VERSION 3.20)
project(duoview_host_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(duoview_tests duoview_tests.cpp)
target_include_directories(duoview_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(MSVC)
  target_compile_options(duoview_tests PRIVATE /W4 /WX /EHsc)
else()
  target_compile_options(duoview_tests PRIVATE -Wall -Wextra -Werror)
endif()
