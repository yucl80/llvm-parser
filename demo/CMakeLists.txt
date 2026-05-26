cmake_minimum_required(VERSION 3.10)

project(demo LANGUAGES CXX)

# Use C++17 by default
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer out-of-source builds. To build:
#   mkdir build && cd build && cmake .. && cmake --build .

add_executable(demo demo.cpp)

# Recommended compiler warnings
if(MSVC)
  target_compile_options(demo PRIVATE /W4 /permissive-)
else()
  target_compile_options(demo PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS demo RUNTIME DESTINATION bin)
