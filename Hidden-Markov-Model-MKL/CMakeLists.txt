cmake_minimum_required(VERSION 3.16)
project(hmm_mkl VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Build Options
# ============================================================================

option(HMM_BUILD_TESTS "Build tests and benchmarks" ON)
option(HMM_USE_OPENMP "Enable OpenMP parallelization" ON)

# ============================================================================
# Compiler Detection
# ============================================================================

set(COMPILER_IS_ICX FALSE)
set(COMPILER_IS_MSVC FALSE)

if(CMAKE_CXX_COMPILER_ID MATCHES "IntelLLVM")
    set(COMPILER_IS_ICX TRUE)
    message(STATUS "Compiler: Intel ICX (IntelLLVM)")
elseif(MSVC)
    set(COMPILER_IS_MSVC TRUE)
    message(STATUS "Compiler: MSVC ${MSVC_VERSION}")
endif()

# ============================================================================
# Find Intel MKL
# ============================================================================

# Try MKLConfig first (modern approach)
find_package(MKL CONFIG QUIET)

if(MKL_FOUND)
    message(STATUS "Found MKL via MKLConfig: ${MKL_ROOT}")
    set(MKL_LIBS MKL::MKL)
else()
    # Fallback: Manual MKL detection
    message(STATUS "MKLConfig not found, trying manual detection...")
    
    # Common MKL paths
    set(MKL_SEARCH_PATHS
        "$ENV{MKLROOT}"
        "$ENV{ONEAPI_ROOT}/mkl/latest"
        "C:/Program Files (x86)/Intel/oneAPI/mkl/latest"
        "/opt/intel/oneapi/mkl/latest"
        "/opt/intel/mkl"
    )
    
    find_path(MKL_INCLUDE_DIR mkl.h
        PATHS ${MKL_SEARCH_PATHS}
        PATH_SUFFIXES include
    )
    
    if(WIN32)
        find_library(MKL_CORE_LIB mkl_rt
            PATHS ${MKL_SEARCH_PATHS}
            PATH_SUFFIXES lib/intel64 lib
        )
        set(MKL_LIBS ${MKL_CORE_LIB})
    else()
        find_library(MKL_CORE_LIB mkl_rt
            PATHS ${MKL_SEARCH_PATHS}
            PATH_SUFFIXES lib/intel64 lib
        )
        set(MKL_LIBS ${MKL_CORE_LIB} pthread m dl)
    endif()
    
    if(MKL_INCLUDE_DIR AND MKL_CORE_LIB)
        message(STATUS "Found MKL manually:")
        message(STATUS "  Include: ${MKL_INCLUDE_DIR}")
        message(STATUS "  Library: ${MKL_CORE_LIB}")
        set(MKL_FOUND TRUE)
    else()
        message(FATAL_ERROR "Intel MKL not found. Set MKLROOT environment variable.")
    endif()
endif()

# ============================================================================
# Find OpenMP
# ============================================================================

if(HMM_USE_OPENMP)
    find_package(OpenMP)
    if(OpenMP_CXX_FOUND)
        message(STATUS "Found OpenMP: ${OpenMP_CXX_VERSION}")
    else()
        message(WARNING "OpenMP not found, parallel features disabled")
    endif()
endif()

# ============================================================================
# Header-only library interface
# ============================================================================

add_library(hmm_mkl INTERFACE)
target_include_directories(hmm_mkl INTERFACE 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

if(MKL_FOUND)
    if(TARGET MKL::MKL)
        target_link_libraries(hmm_mkl INTERFACE MKL::MKL)
    else()
        target_include_directories(hmm_mkl INTERFACE ${MKL_INCLUDE_DIR})
        target_link_libraries(hmm_mkl INTERFACE ${MKL_LIBS})
    endif()
endif()

if(OpenMP_CXX_FOUND)
    target_link_libraries(hmm_mkl INTERFACE OpenMP::OpenMP_CXX)
endif()

# ============================================================================
# Compiler Flags (per-config, not global)
# ============================================================================

# Intel ICX (IntelLLVM) - Maximum performance
if(COMPILER_IS_ICX)
    target_compile_options(hmm_mkl INTERFACE
        $<$<CONFIG:Release>:/O3 /fp:fast /QxHost /Qipo /Qfma /Qopt-zmm-usage:high>
        $<$<CONFIG:RelWithDebInfo>:/O2 /fp:fast /QxHost /Zi>
        $<$<CONFIG:Debug>:/Od /Zi>
    )
    target_link_options(hmm_mkl INTERFACE
        $<$<CONFIG:Release>:/Qipo>
    )
    
# MSVC
elseif(COMPILER_IS_MSVC)
    target_compile_options(hmm_mkl INTERFACE
        $<$<CONFIG:Release>:/O2 /fp:fast /arch:AVX2 /GL>
        $<$<CONFIG:RelWithDebInfo>:/O2 /fp:fast /arch:AVX2 /Zi>
        $<$<CONFIG:Debug>:/Od /Zi>
    )
    target_link_options(hmm_mkl INTERFACE
        $<$<CONFIG:Release>:/LTCG>
    )
endif()

# ============================================================================
# Test / Benchmark Executables
# ============================================================================

if(HMM_BUILD_TESTS)
    # Gaussian HMM test
    add_executable(hmm_test hmm_test.cpp)
    target_link_libraries(hmm_test PRIVATE hmm_mkl)
    
    # Student-t AR-HMM test
    add_executable(hmm_student_t_test hmm_student_t_test.cpp)
    target_link_libraries(hmm_student_t_test PRIVATE hmm_mkl)
    
    # Windows: Copy MKL DLLs to output directory
    if(WIN32 AND MKL_FOUND AND NOT TARGET MKL::MKL)
        get_filename_component(MKL_LIB_DIR "${MKL_CORE_LIB}" DIRECTORY)
        get_filename_component(MKL_ROOT_DIR "${MKL_LIB_DIR}" DIRECTORY)
        set(MKL_BIN_DIR "${MKL_ROOT_DIR}/../bin")
        
        # Normalize path
        get_filename_component(MKL_BIN_DIR "${MKL_BIN_DIR}" ABSOLUTE)
        
        if(EXISTS "${MKL_BIN_DIR}/mkl_rt.2.dll")
            add_custom_command(TARGET hmm_test POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MKL_BIN_DIR}/mkl_rt.2.dll"
                "$<TARGET_FILE_DIR:hmm_test>"
                COMMENT "Copying MKL runtime DLL"
            )
            add_custom_command(TARGET hmm_student_t_test POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MKL_BIN_DIR}/mkl_rt.2.dll"
                "$<TARGET_FILE_DIR:hmm_student_t_test>"
                COMMENT "Copying MKL runtime DLL"
            )
        endif()
    endif()
endif()

# ============================================================================
# 🌟 HFT Live Engine Executable (Binance WSS + ZeroMQ + MKL)
# ============================================================================
# این بلاک فایل main.cpp ما را به یک موتور زنده تبدیل می‌کند
add_executable(hmm_engine main.cpp)

# پیوند زدن کتابخانه ریاضی HMM (MKL/OpenMP) به همراه ابزارهای شبکه (ZMQ و OpenSSL)
target_link_libraries(hmm_engine 
    PRIVATE 
    hmm_mkl
    zmq
    ssl
    crypto
)
# ============================================================================
# Installation
# ============================================================================

include(GNUInstallDirs)

install(TARGETS hmm_mkl
    EXPORT hmm_mkl-targets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES 
    hmm_mkl.hpp
    hmm_student_t.hpp
    eemd_mkl.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT hmm_mkl-targets
    FILE hmm_mkl-targets.cmake
    NAMESPACE hmm_mkl::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hmm_mkl
)

# ============================================================================
# Summary
# ============================================================================

message(STATUS "")
message(STATUS "HMM-MKL Configuration:")
message(STATUS "  Version:     ${PROJECT_VERSION}")
message(STATUS "  Compiler:    ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "  MKL:         ${MKL_FOUND}")
message(STATUS "  OpenMP:      ${OpenMP_CXX_FOUND}")
message(STATUS "  Build tests: ${HMM_BUILD_TESTS}")
message(STATUS "")
message(STATUS "Build with:")
message(STATUS "  cmake --build . --config Release")
message(STATUS "")