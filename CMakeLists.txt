cmake_minimum_required(VERSION 3.19)
project(circular_buffer C)

set(CMAKE_C_STANDARD 99)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(circular_buffer
        CircularBuffer.c
        CircularBuffer.h
        main.c)

target_link_libraries(circular_buffer
        pthread)