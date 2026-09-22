cmake_minimum_required(VERSION 3.16)
project(CrowdIntelBot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Options ───────────────────────────────────────────────────────────────────
option(CROWDINTEL_MARCH_NATIVE "Optimize for the build host CPU (-march=native)" ON)
option(CROWDINTEL_NETWORK      "Build the live network layer (needs libcurl + OpenSSL)" ON)
option(CROWDINTEL_LTO          "Enable link-time optimization" ON)

# ── Compiler flags ────────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CROWDINTEL_FLAGS
    -O3
    -funroll-loops
    -fno-plt
    -fvisibility=hidden
    -Wall
    -Wextra
    -Wno-unused-parameter
)
if(CROWDINTEL_MARCH_NATIVE)
    list(APPEND CROWDINTEL_FLAGS -march=native)
endif()
add_compile_options(${CROWDINTEL_FLAGS})
if(CROWDINTEL_LTO)
    add_link_options(-flto)
endif()

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# libsecp256k1 (required — ECDSA core)
find_library(SECP256K1_LIBRARY NAMES secp256k1)
find_path(SECP256K1_INCLUDE_DIR secp256k1.h)
if(NOT SECP256K1_LIBRARY OR NOT SECP256K1_INCLUDE_DIR)
    if(DEFINED ENV{SECP256K1_ROOT})
        set(SECP256K1_LIBRARY "$ENV{SECP256K1_ROOT}/.libs/libsecp256k1.a")
        set(SECP256K1_INCLUDE_DIR "$ENV{SECP256K1_ROOT}/include")
    endif()
endif()
if(NOT SECP256K1_LIBRARY OR NOT SECP256K1_INCLUDE_DIR)
    message(FATAL_ERROR "libsecp256k1 not found. Install it or set SECP256K1_ROOT "
                        "(see README: cmake build with -DSECP256K1_ENABLE_MODULE_RECOVERY=ON).")
endif()
message(STATUS "libsecp256k1: ${SECP256K1_LIBRARY}")

# OpenSSL + libcurl (optional — only the live network layer needs them)
set(HAVE_NETWORK OFF)
if(CROWDINTEL_NETWORK)
    find_package(OpenSSL)
    find_package(CURL QUIET)
    if(NOT CURL_FOUND AND DEFINED ENV{CURL_ROOT})
        set(CURL_LIBRARY "$ENV{CURL_ROOT}/lib/libcurl.a")
        set(CURL_INCLUDE_DIR "$ENV{CURL_ROOT}/include")
        if(EXISTS "${CURL_LIBRARY}" AND EXISTS "${CURL_INCLUDE_DIR}/curl/curl.h")
            set(CURL_FOUND TRUE)
        endif()
    endif()
    if(OpenSSL_FOUND AND CURL_FOUND)
        set(HAVE_NETWORK ON)
        message(STATUS "network layer: ON (curl ${CURL_LIBRARY})")
    else()
        message(STATUS "network layer: OFF (libcurl/OpenSSL not found — "
                       "tests and benchmarks still build; set CURL_ROOT/OpenSSL_ROOT to enable)")
    endif()
else()
    message(STATUS "network layer: OFF (CROWDINTEL_NETWORK=OFF)")
endif()

# ── Include paths ─────────────────────────────────────────────────────────────
set(CROWDINTEL_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto
    ${CMAKE_CURRENT_SOURCE_DIR}/../alpha/crowdintel
    ${CMAKE_CURRENT_SOURCE_DIR}/../alpha/strategy
    ${SECP256K1_INCLUDE_DIR}
)
if(HAVE_NETWORK)
    list(APPEND CROWDINTEL_INCLUDES ${CURL_INCLUDE_DIR} ${OPENSSL_INCLUDE_DIR})
endif()

# ── Core objects shared by every target (no network deps) ────────────────────
add_library(crowdintel_core OBJECT
    ../alpha/crowdintel/alpha_parser.cpp
)
target_include_directories(crowdintel_core PRIVATE ${CROWDINTEL_INCLUDES})

macro(crowdintel_link_secp TGT)
    target_include_directories(${TGT} PRIVATE ${CROWDINTEL_INCLUDES})
    target_link_libraries(${TGT} PRIVATE ${SECP256K1_LIBRARY} Threads::Threads)
endmacro()

# ── Main bot (network layer conditional) ─────────────────────────────────────
add_executable(crowdintel_bot src/main_hot_path.cpp)
crowdintel_link_secp(crowdintel_bot)
target_link_libraries(crowdintel_bot PRIVATE crowdintel_core)
if(HAVE_NETWORK)
    target_compile_definitions(crowdintel_bot PRIVATE CROWDINTEL_HAVE_NETWORK=1)
    target_link_libraries(crowdintel_bot PRIVATE
        ${CURL_LIBRARY} OpenSSL::SSL OpenSSL::Crypto)
else()
    # Offline build: force mock mode at the entrypoint level.
    target_compile_definitions(crowdintel_bot PRIVATE CROWDINTEL_FORCE_MOCK=1)
endif()
set_target_properties(crowdintel_bot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Crypto known-answer tests + golden vectors ───────────────────────────────
add_executable(test_signer crypto/test_signer.cpp)
crowdintel_link_secp(test_signer)
set_target_properties(test_signer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Unit tests (SPSC, book, Kelly, amounts, wire body, HMAC, …) ──────────────
add_executable(test_core ../tests/unit/test_core.cpp)
crowdintel_link_secp(test_core)
target_link_libraries(test_core PRIVATE crowdintel_core)
set_target_properties(test_core PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Latency benchmark ─────────────────────────────────────────────────────────
add_executable(latency_bench ../tests/benchmarks/latency_bench.cpp)
crowdintel_link_secp(latency_bench)
set_target_properties(latency_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── L2 backtester ─────────────────────────────────────────────────────────────
add_executable(l2_backtester ../tests/replay/l2_backtester.cpp)
crowdintel_link_secp(l2_backtester)
set_target_properties(l2_backtester PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── CTest ─────────────────────────────────────────────────────────────────────
enable_testing()
add_test(NAME crypto_kat      COMMAND $<TARGET_FILE:test_signer>)
add_test(NAME core_units      COMMAND $<TARGET_FILE:test_core>)
add_test(NAME latency_bench   COMMAND $<TARGET_FILE:latency_bench>)
add_test(NAME backtester_smoke COMMAND $<TARGET_FILE:l2_backtester>
             ${CMAKE_CURRENT_SOURCE_DIR}/../tests/replay/sample_ticks.csv)
