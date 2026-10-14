cmake_minimum_required(VERSION 3.15)
project(LowLatencyOrderBook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

# Platform socket libraries and Windows-specific compile flags
set(SOCKET_LIBS "")
if (WIN32)
    set(SOCKET_LIBS ws2_32)
    # Required for mingw-std-threads (targets Windows 7+)
    add_compile_definitions(_WIN32_WINNT=0x0601 WINVER=0x0601)
    # MinGW win32 thread model: -mthreads enables POSIX-compatible threading primitives
    add_compile_options(-mthreads)
    add_link_options(-mthreads)
endif()

# Core library sources
set(LIB_SOURCES
    src/codec/BinaryCodec.cpp
    src/codec/TextCodec.cpp
    src/net/FrameReader.cpp
    src/net/Poller.cpp
    src/net/Dispatcher.cpp
    src/core/Worker.cpp
    src/client/ClientSession.cpp
)

add_library(exchange_core STATIC ${LIB_SOURCES})
target_link_libraries(exchange_core ${SOCKET_LIBS})

# Server executable
add_executable(exchange_server src/server_main.cpp)
target_link_libraries(exchange_server exchange_core ${SOCKET_LIBS})

# Client executable
add_executable(exchange_client src/client_main.cpp)
target_link_libraries(exchange_client exchange_core ${SOCKET_LIBS})

# Tests
enable_testing()
add_executable(test_suite tests/test_all.cpp)
target_include_directories(test_suite PRIVATE tests)
target_link_libraries(test_suite exchange_core ${SOCKET_LIBS})
add_test(NAME AllTests COMMAND test_suite)

# Automated C++ Demonstration
add_executable(demo_automated src/demo_main.cpp)
target_link_libraries(demo_automated exchange_core ${SOCKET_LIBS})

# Section 8 Benchmark Suite
add_executable(benchmark_suite src/benchmark_main.cpp)
target_link_libraries(benchmark_suite exchange_core ${SOCKET_LIBS})

