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
    -O3 -march=native -funsafe-math-optimizations -fno-math-errno -fno-trapping-math -fno-signed-zeros
)

# ============================================================================
# SSA Benchmark — retired in Phase-2
# ----------------------------------------------------------------------------
# The Recursive-SVD / SsaPipeline baseline benchmarked by benchmark/bench_main.cpp
# has been superseded by the new Phase-1/Phase-2 dsp_math / FilterPipeline
# architecture. The benchmark code is preserved on disk as code archaeology but
# is no longer built into the consolidated Docker image — saving toolchain time
# and eliminating CircularPriceBuffer / SsaResult dependencies that have been
# removed from ssa_types.hpp.
#
# To re-enable: re-add the file list above and restore CircularPriceBuffer /
# SsaResult into ssa_types.hpp (or carve them out into benchmark/legacy.hpp).
# ============================================================================
