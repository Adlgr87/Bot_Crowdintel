cmake_minimum_required(VERSION 3.16)
project(CrowdIntelBot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CROWDINTEL_NETWORK "Build live curl/OpenSSL transports" ON)
option(CROWDINTEL_LTO "Enable compiler-supported interprocedural optimization" ON)
option(CROWDINTEL_SANITIZERS "Backward-compatible alias for ASan + UBSan" OFF)
option(CROWDINTEL_ASAN "Enable AddressSanitizer" OFF)
option(CROWDINTEL_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(CROWDINTEL_TSAN "Enable ThreadSanitizer" OFF)
set(CROWDINTEL_CPU_TARGET "portable" CACHE STRING
    "CPU target: portable, x86-64-v2, x86-64-v3, or native (local-only)")
set_property(CACHE CROWDINTEL_CPU_TARGET PROPERTY STRINGS
    portable x86-64-v2 x86-64-v3 native)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

if(SECP256K1_ROOT OR DEFINED ENV{SECP256K1_ROOT})
    if(SECP256K1_ROOT)
        set(_secp_root "${SECP256K1_ROOT}")
    else()
        set(_secp_root "$ENV{SECP256K1_ROOT}")
    endif()
    # An explicit root is a supply-chain boundary, not a hint behind whatever
    # version happens to be installed globally or cached from an older build.
    unset(SECP256K1_LIBRARY CACHE)
    unset(SECP256K1_INCLUDE_DIR CACHE)
    find_library(SECP256K1_LIBRARY NAMES secp256k1
        PATHS "${_secp_root}/build/lib" "${_secp_root}/lib" "${_secp_root}/.libs"
        NO_DEFAULT_PATH)
    find_path(SECP256K1_INCLUDE_DIR secp256k1.h
        PATHS "${_secp_root}/include" NO_DEFAULT_PATH)
else()
    find_library(SECP256K1_LIBRARY NAMES secp256k1)
    find_path(SECP256K1_INCLUDE_DIR secp256k1.h)
endif()
if(NOT SECP256K1_LIBRARY OR NOT SECP256K1_INCLUDE_DIR)
    message(FATAL_ERROR "libsecp256k1 with recovery support not found; pass "
                        "-DSECP256K1_LIBRARY=... -DSECP256K1_INCLUDE_DIR=...")
endif()
message(STATUS "libsecp256k1: ${SECP256K1_LIBRARY}")

set(HAVE_NETWORK OFF)
if(CROWDINTEL_NETWORK)
    find_package(OpenSSL QUIET)
    find_package(CURL QUIET)
    if(OpenSSL_FOUND AND CURL_FOUND)
        set(HAVE_NETWORK ON)
        message(STATUS "network layer: ON")
    else()
        message(FATAL_ERROR
            "CROWDINTEL_NETWORK=ON requires libcurl and OpenSSL development packages; "
            "use -DCROWDINTEL_NETWORK=OFF for an explicit offline build")
    endif()
endif()

add_library(crowdintel_options INTERFACE)
target_compile_options(crowdintel_options INTERFACE
    $<$<CONFIG:Release>:-O3>
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -fno-plt -fvisibility=hidden)
if(CROWDINTEL_CPU_TARGET STREQUAL "native")
    target_compile_options(crowdintel_options INTERFACE -march=native)
    message(WARNING "-march=native binaries must not be copied to another CPU class")
elseif(CROWDINTEL_CPU_TARGET MATCHES "^x86-64-v[23]$")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        target_compile_options(crowdintel_options INTERFACE
            -march=${CROWDINTEL_CPU_TARGET})
    else()
        message(FATAL_ERROR "${CROWDINTEL_CPU_TARGET} requested on non-x86 target")
    endif()
elseif(NOT CROWDINTEL_CPU_TARGET STREQUAL "portable")
    message(FATAL_ERROR "Unknown CROWDINTEL_CPU_TARGET=${CROWDINTEL_CPU_TARGET}")
endif()

if(CROWDINTEL_SANITIZERS)
    set(CROWDINTEL_ASAN ON)
    set(CROWDINTEL_UBSAN ON)
endif()
if(CROWDINTEL_TSAN AND (CROWDINTEL_ASAN OR CROWDINTEL_UBSAN))
    message(FATAL_ERROR "TSan cannot be combined with ASan/UBSan in this build")
endif()
set(_sanitizers "")
if(CROWDINTEL_ASAN)
    list(APPEND _sanitizers address)
endif()
if(CROWDINTEL_UBSAN)
    list(APPEND _sanitizers undefined)
endif()
if(CROWDINTEL_TSAN)
    list(APPEND _sanitizers thread)
endif()
if(_sanitizers)
    list(JOIN _sanitizers "," _sanitizer_list)
    target_compile_options(crowdintel_options INTERFACE
        -fsanitize=${_sanitizer_list} -fno-omit-frame-pointer -O1)
    target_link_options(crowdintel_options INTERFACE
        -fsanitize=${_sanitizer_list})
    message(STATUS "sanitizers: ${_sanitizer_list}")
endif()

if(CROWDINTEL_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)
    if(IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "IPO/LTO: ON")
    else()
        message(WARNING "IPO/LTO unsupported: ${IPO_ERROR}")
    endif()
endif()

set(CROWDINTEL_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto
    ${CMAKE_CURRENT_SOURCE_DIR}/../alpha/crowdintel
    ${CMAKE_CURRENT_SOURCE_DIR}/../alpha/strategy
    ${SECP256K1_INCLUDE_DIR})

function(crowdintel_target target)
    target_include_directories(${target} PRIVATE ${CROWDINTEL_INCLUDES})
    target_link_libraries(${target} PRIVATE
        crowdintel_options ${SECP256K1_LIBRARY} Threads::Threads)
endfunction()

add_executable(crowdintel_bot src/main_hot_path.cpp)
crowdintel_target(crowdintel_bot)
if(HAVE_NETWORK)
    target_compile_definitions(crowdintel_bot PRIVATE CROWDINTEL_HAVE_NETWORK=1)
    target_link_libraries(crowdintel_bot PRIVATE CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
else()
    target_compile_definitions(crowdintel_bot PRIVATE CROWDINTEL_FORCE_MOCK=1)
endif()
set_target_properties(crowdintel_bot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_signer crypto/test_signer.cpp)
crowdintel_target(test_signer)
set_target_properties(test_signer PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_core ../tests/unit/test_core.cpp)
crowdintel_target(test_core)
if(HAVE_NETWORK)
    target_compile_definitions(test_core PRIVATE CROWDINTEL_HAVE_NETWORK=1)
    target_link_libraries(test_core PRIVATE
        CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
endif()
set_target_properties(test_core PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(latency_bench ../tests/benchmarks/latency_bench.cpp)
crowdintel_target(latency_bench)
set_target_properties(latency_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(l2_backtester ../tests/replay/l2_backtester.cpp)
crowdintel_target(l2_backtester)
set_target_properties(l2_backtester PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_layers ../tests/unit/test_layers.cpp)
crowdintel_target(test_layers)
if(HAVE_NETWORK)
    target_compile_definitions(test_layers PRIVATE CROWDINTEL_HAVE_NETWORK=1)
    target_link_libraries(test_layers PRIVATE
        CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
endif()
set_target_properties(test_layers PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── AVX2 detection ─────────────────────────────────────────────────────────────
# CI uses -DCROWDINTEL_CPU_TARGET=portable (no AVX2 by default). CfC network
# uses AVX2+FMA intrinsics. Detect availability before building those targets.
set(CMAKE_REQUIRED_FLAGS_SAVE "${CMAKE_REQUIRED_FLAGS}")
set(CMAKE_REQUIRED_FLAGS "-mavx2 -mfma ${CMAKE_REQUIRED_FLAGS}")
check_cxx_source_compiles("
    #include <immintrin.h>
    int main() {
        __m256 a = _mm256_set1_ps(1.0f);
        __m256 b = _mm256_fmadd_ps(a, a, a);
        (void)b; return 0;
    }
" HAVE_AVX2_INTRINSICS)
set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS_SAVE}")

# ── Phase 1: OFI + Binance WS + Evidence Adapter tests ──────────────────────────
# OFI uses only SSE4.2 (portable across all x86-64). No AVX2 required.
add_executable(test_ofi ../tests/unit/test_ofi.cpp)
crowdintel_target(test_ofi)
set_target_properties(test_ofi PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Phase 2: CfC Network tests ─────────────────────────────────────────────────
# Only build when AVX2+FMA intrinsics are available.
if(HAVE_AVX2_INTRINSICS)
    # Enable AVX2+FMA for CfC test targets (intrinsics are used unconditionally
    # in cfc_network.hpp). check_cxx_source_compiles only tests compile ability;
    # we must also pass the flags to actual targets.
    add_executable(test_cfc_kat ../tests/unit/test_cfc_kat.cpp)
    crowdintel_target(test_cfc_kat)
    target_compile_options(test_cfc_kat PRIVATE -mavx2 -mfma)
    set_target_properties(test_cfc_kat PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    # Benchmark: latency < 3μs p50, < 5μs p99
    add_executable(bench_cfc ../tests/benchmarks/bench_cfc.cpp)
    crowdintel_target(bench_cfc)
    target_compile_options(bench_cfc PRIVATE -mavx2 -mfma)
    set_target_properties(bench_cfc PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    # bench_cfc must run from the repo root to find the model file
    set_target_properties(bench_cfc PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..)

    # Integration test (also requires AVX2 for CfC)
    add_executable(test_pipeline ../tests/integration/test_pipeline.cpp)
    crowdintel_target(test_pipeline)
    target_compile_options(test_pipeline PRIVATE -mavx2 -mfma)
    set_target_properties(test_pipeline PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set_target_properties(test_pipeline PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..)
else()
    message(STATUS "AVX2 not available — skipping CfC tests/benchmarks")
endif()

enable_testing()
add_test(NAME crypto_kat COMMAND $<TARGET_FILE:test_signer>)
add_test(NAME core_units COMMAND $<TARGET_FILE:test_core>)
add_test(NAME layer_units COMMAND $<TARGET_FILE:test_layers>)
add_test(NAME ofi_units COMMAND $<TARGET_FILE:test_ofi>)
if(HAVE_AVX2_INTRINSICS)
    add_test(NAME cfc_kat COMMAND $<TARGET_FILE:test_cfc_kat>)
    add_test(NAME pipeline_integration COMMAND $<TARGET_FILE:test_pipeline>)
endif()
add_test(NAME backtester_smoke COMMAND $<TARGET_FILE:l2_backtester>
         ${CMAKE_CURRENT_SOURCE_DIR}/../tests/replay/sample_ticks.csv)
# bench_cfc and latency_bench are benchmarks, not correctness tests; CI invokes them explicitly.
