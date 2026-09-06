cmake_minimum_required(VERSION 3.16)
project(goggles_capture CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_executable(goggles_capture
        src/common.cpp
        src/goggles_capture.cpp
        src/usb_gadget.cpp
        include/common.h
        include/goggles_capture.h
        include/usb_gadget.h)
target_include_directories(goggles_capture PRIVATE include)
target_compile_options(goggles_capture PRIVATE -Wall -Wextra)
target_link_libraries(goggles_capture PRIVATE Threads::Threads)
