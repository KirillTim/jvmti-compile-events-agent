 cmake_minimum_required(VERSION 3.9)
project(perf-map-agent)

set(CMAKE_CXX_STANDARD 11)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Ofast -stdlib=libc++")

include_directories(src)

find_package(JNI)
INCLUDE_DIRECTORIES(${JAVA_INCLUDE_PATH})
INCLUDE_DIRECTORIES(${JAVA_INCLUDE_PATH2})

add_library(perfmap SHARED
        src/agent.cpp
        src/utils.cpp
        src/utils.h
        src/thread_info.cpp
        src/thread_info.h
        src/logger.cpp
        src/logger.h)