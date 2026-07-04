cmake_minimum_required(VERSION 3.16)
project(ssa_engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Find Dependencies
# ============================================================================

find_package(PkgConfig REQUIRED)

# ZeroMQ (libzmq + cppzmq header-only bindings)
pkg_check_modules(ZMQ REQUIRED libzmq)

find_path(CPPZMQ_INCLUDE_DIR zmq.hpp
    HINTS ${ZMQ_INCLUDE_DIRS}
    PATHS /usr/include /usr/local/include
)
if(NOT CPPZMQ_INCLUDE_DIR)
    message(FATAL_ERROR "cppzmq header (zmq.hpp) not found. Install libzmq3-dev and cppzmq-dev.")
endif()

# libcurl for ClickHouse HTTP API
find_package(CURL REQUIRED)

# ============================================================================
# SSA Engine Executable
# ============================================================================

add_executable(ssa_engine main.cpp)

target_include_directories(ssa_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ZMQ_INCLUDE_DIRS}
    ${CPPZMQ_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(ssa_engine PRIVATE
    ${ZMQ_LIBRARIES}
    ${CURL_LIBRARIES}
    pthread
)

target_compile_options(ssa_engine PRIVATE
    -O3 -march=native -ffast-math
)

# ============================================================================
# SSA Benchmark (standalone — no ZMQ, no curl, no network deps)
# ============================================================================

add_executable(ssa_benchmark benchmark/bench_main.cpp)

target_include_directories(ssa_benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/benchmark
)

target_link_libraries(ssa_benchmark PRIVATE
    pthread
    m
)

target_compile_options(ssa_benchmark PRIVATE
    -O3 -march=native -ffast-math
)
