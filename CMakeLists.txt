cmake_minimum_required(VERSION 3.16)
project(edgedoc_cam LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The V4L2 capture program requires Linux")
endif()

add_executable(edgedoc-v4l2-capture src/v4l2_capture.cpp)
target_compile_features(edgedoc-v4l2-capture PRIVATE cxx_std_17)
target_compile_options(edgedoc-v4l2-capture PRIVATE -Wall -Wextra -Wpedantic)
