cmake_minimum_required(VERSION 3.16)
project(goggles_capture CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_executable(goggles_capture
        goggles_capture.cpp
        goggles_capture.h)
target_compile_options(goggles_capture PRIVATE -Wall)
target_link_libraries(goggles_capture PRIVATE Threads::Threads)
