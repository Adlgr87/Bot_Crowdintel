# CMakeLists.txt for Bot CrowdIntel Core (Hot Path)
# Optimized for Ultra-Low Latency with LTO, PGO, and CPU-specific instructions

cmake_minimum_required(VERSION 3.16)
project(CrowdIntelBot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Compiler Flags for Maximum Performance ---
# -O3: Aggressive optimization
# -march=native: Use all instructions available on the host CPU (AVX2, etc)
# -flto: Link Time Optimization (cross-module optimization)
# -fno-rtti: Remove RTTI overhead (exceptions retained for OpenSSL 3.x)
add_compile_options(
    -O3
    -march=native
    -flto
    -fno-rtti
    -funroll-loops
    -DNDEBUG
)

# Linker Flags for LTO
add_link_options(-flto)

# --- Find Dependencies (CRITICAL for real build) ---
# Try standard find_package first; fall back to manual paths from conda env
find_package(CURL QUIET COMPONENTS libcurl)
find_package(OpenSSL REQUIRED)

# Manual fallback for CURL (conda env path)
if(NOT CURL_FOUND)
    set(CURL_CONDA_PREFIX "$ENV{CURL_CONDA_PREFIX}")
    if(NOT CURL_CONDA_PREFIX)
        set(CURL_CONDA_PREFIX "/home/adlg/text-generation-webui/text-generation-webui/installer_files/conda")
    endif()
    set(CURL_LIBRARY "${CURL_CONDA_PREFIX}/lib/libcurl.so")
    set(CURL_INCLUDE_DIR "${CURL_CONDA_PREFIX}/include")
    if(EXISTS ${CURL_LIBRARY} AND EXISTS ${CURL_INCLUDE_DIR}/curl/curl.h)
        set(CURL_FOUND TRUE)
        message(STATUS "CURL found via conda fallback: ${CURL_LIBRARY}")
    else()
        message(FATAL_ERROR "CURL not found. Set CURL_CONDA_PREFIX or install libcurl-dev.")
    endif()
endif()

# --- Source Files ---
file(GLOB_RECURSE CORE_SOURCES
    "src/*.cpp"
    "../alpha/crowdintel/*.cpp"
)

# Include directories
include_directories(
    "./include"
    "./src"
    "./crypto"
    "../alpha/crowdintel"
    "../alpha/strategy"
    ${CURL_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
)

# --- Main Executable ---
add_executable(crowdintel_bot ${CORE_SOURCES})

# Link libraries
target_link_libraries(crowdintel_bot
    ${CURL_LIBRARY}
    OpenSSL::Crypto
    OpenSSL::SSL
)

# RPATH to find libraries
set_target_properties(crowdintel_bot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# --- Test Targets (built from core/ root, tests/ is sibling) ---
# EIP-712 Signer Test
add_executable(test_signer crypto/test_signer.cpp)
target_link_libraries(test_signer
    OpenSSL::Crypto
    OpenSSL::SSL
)
set_target_properties(test_signer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Latency Benchmark
add_executable(latency_bench ../tests/benchmarks/latency_bench.cpp)
target_link_libraries(latency_bench
    OpenSSL::Crypto
    OpenSSL::SSL
)
set_target_properties(latency_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# L2 Backtester
add_executable(l2_backtester ../tests/replay/l2_backtester.cpp)
target_link_libraries(l2_backtester
    OpenSSL::Crypto
    OpenSSL::SSL
)
set_target_properties(l2_backtester PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)