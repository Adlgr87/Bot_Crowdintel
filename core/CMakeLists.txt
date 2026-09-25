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

# --- Find Dependencies ---
find_package(CURL QUIET COMPONENTS libcurl)
find_package(OpenSSL REQUIRED)
find_library(SECP256K1_LIBRARY secp256k1)
find_path(SECP256K1_INCLUDE_DIR secp256k1.h)

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

# Manual fallback for libsecp256k1 (if not found via find_library)
if(NOT SECP256K1_LIBRARY)
    set(SECP256K1_ROOT "$ENV{SECP256K1_ROOT}")
    if(NOT SECP256K1_ROOT)
        set(SECP256K1_ROOT "/tmp/secp256k1")
    endif()
    set(SECP256K1_LIBRARY "${SECP256K1_ROOT}/.libs/libsecp256k1.a")
    set(SECP256K1_INCLUDE_DIR "${SECP256K1_ROOT}/include")
    if(EXISTS ${SECP256K1_LIBRARY} AND EXISTS ${SECP256K1_INCLUDE_DIR}/secp256k1.h)
        message(STATUS "libsecp256k1 found via fallback: ${SECP256K1_LIBRARY}")
    else()
        message(WARNING "libsecp256k1 not found. ECDSA signing will use OpenSSL fallback.")
    endif()
endif()

# --- Source Files ---
# Production sources: exclude main_hot_path.cpp (demo entrypoint, not production)
# main_hot_path.cpp is a demo that requires env vars set at runtime.
# It is built as a separate optional target below.
file(GLOB_RECURSE CORE_SOURCES
    "src/*.cpp"
    "../alpha/crowdintel/*.cpp"
)
# Remove main_hot_path.cpp from production sources (use main_prod.cpp instead)
list(FILTER CORE_SOURCES EXCLUDE REGEX ".*main_hot_path\\.cpp$")
# Ensure main_prod.cpp is included (not always picked up by GLOB if glob cache is stale)
list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main_prod.cpp")

# --- Demo / Development Target (not built by default) ---
# opt-in demo binary that requires env vars (CLOB_API_KEY, CLOB_SECRET,
# CLOB_PASSPHRASE, BOT_PRIVATE_KEY_HEX) at runtime.
option(BUILD_DEMO "Build the main_hot_path demo binary (not for production)" OFF)

# Include directories
include_directories(
    "./include"
    "./src"
    "./crypto"
    "../alpha/crowdintel"
    "../alpha/strategy"
    ${CURL_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
    ${SECP256K1_INCLUDE_DIR}
)

# --- Main Executable ---
add_executable(crowdintel_bot ${CORE_SOURCES})

# Link libraries
if(SECP256K1_LIBRARY)
    target_link_libraries(crowdintel_bot
        ${SECP256K1_LIBRARY}
        ${CURL_LIBRARY}
        OpenSSL::Crypto
        OpenSSL::SSL
    )
else()
    target_link_libraries(crowdintel_bot
        ${CURL_LIBRARY}
        OpenSSL::Crypto
        OpenSSL::SSL
    )
endif()

# RPATH to find libraries
set_target_properties(crowdintel_bot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# --- Optional Demo Target ---
# main_hot_path.cpp is a demo entry point. It #includes execution_engine.cpp directly,
# so it has its own main(). Only built when BUILD_DEMO=ON.
if(BUILD_DEMO)
    add_executable(crowdintel_bot_demo src/main_hot_path.cpp)
    if(SECP256K1_LIBRARY)
        target_link_libraries(crowdintel_bot_demo
            ${SECP256K1_LIBRARY}
            ${CURL_LIBRARY}
            OpenSSL::Crypto
            OpenSSL::SSL
        )
    else()
        target_link_libraries(crowdintel_bot_demo
            ${CURL_LIBRARY}
            OpenSSL::Crypto
            OpenSSL::SSL
        )
    endif()
    target_compile_definitions(crowdintel_bot_demo PRIVATE EXECUTION_ENGINE_HEADER_ONLY=1)
    set_target_properties(crowdintel_bot_demo PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endif()

# --- Test Sources ---
# Collect all test files from tests/ directory (recursively)
file(GLOB_RECURSE TEST_SOURCES
    "../tests/test_*.cpp"
)
# Build each test as a separate executable
foreach(TEST_FILE ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
    add_executable(${TEST_NAME} ${TEST_FILE})
    if(SECP256K1_LIBRARY)
        target_link_libraries(${TEST_NAME}
            ${SECP256K1_LIBRARY}
            ${CURL_LIBRARY}
            OpenSSL::Crypto
            OpenSSL::SSL
        )
    else()
        target_link_libraries(${TEST_NAME}
            ${CURL_LIBRARY}
            OpenSSL::Crypto
            OpenSSL::SSL
        )
    endif()
    target_include_directories(${TEST_NAME} PRIVATE
        "./include"
        "./src"
        "./crypto"
        "../alpha/crowdintel"
        "../alpha/strategy"
        "../tests"
    )
    set_target_properties(${TEST_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    add_test(NAME ${TEST_NAME}
        COMMAND $<TARGET_FILE:${TEST_NAME}>
    )
endforeach()

# --- CTest Integration ---
enable_testing()
add_test(NAME keccak_known_answer
    COMMAND $<TARGET_FILE:test_signer>
)
add_test(NAME latency_benchmark
    COMMAND $<TARGET_FILE:latency_bench>
)

# --- Test Targets (built from core/ root, tests/ is sibling) ---
# EIP-712 Signer Test
add_executable(test_signer crypto/test_signer.cpp)
if(SECP256K1_LIBRARY)
    target_link_libraries(test_signer ${SECP256K1_LIBRARY} OpenSSL::Crypto OpenSSL::SSL)
else()
    target_link_libraries(test_signer OpenSSL::Crypto OpenSSL::SSL)
endif()
set_target_properties(test_signer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Latency Benchmark
add_executable(latency_bench ../tests/benchmarks/latency_bench.cpp)
if(SECP256K1_LIBRARY)
    target_link_libraries(latency_bench ${SECP256K1_LIBRARY} OpenSSL::Crypto OpenSSL::SSL)
else()
    target_link_libraries(latency_bench OpenSSL::Crypto OpenSSL::SSL)
endif()
set_target_properties(latency_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# L2 Backtester
add_executable(l2_backtester ../tests/replay/l2_backtester.cpp)
target_link_libraries(l2_backtester OpenSSL::Crypto OpenSSL::SSL)
set_target_properties(l2_backtester PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)