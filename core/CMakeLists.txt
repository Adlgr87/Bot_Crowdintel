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

add_executable(test_market_metadata ../tests/unit/test_market_metadata.cpp)
crowdintel_target(test_market_metadata)
set_target_properties(test_market_metadata PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_ledger ../tests/unit/test_ledger.cpp)
crowdintel_target(test_ledger)
set_target_properties(test_ledger PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_reconciliation ../tests/unit/test_reconciliation.cpp)
crowdintel_target(test_reconciliation)
if(HAVE_NETWORK)
    target_compile_definitions(test_reconciliation PRIVATE CROWDINTEL_HAVE_NETWORK=1)
    target_link_libraries(test_reconciliation PRIVATE
        CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
endif()
set_target_properties(test_reconciliation PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The preflight is transport-templated, so its tests need no curl at all and
# run in every build.
add_executable(test_preflight ../tests/unit/test_preflight.cpp)
crowdintel_target(test_preflight)
set_target_properties(test_preflight PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Phase 7: mode/role matrix and strict environment validation. Pure
# configuration, so it builds and runs in every variant.
add_executable(test_config ../tests/unit/test_config.cpp)
crowdintel_target(test_config)
set_target_properties(test_config PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_order_heartbeat ../tests/unit/test_order_heartbeat.cpp)
crowdintel_target(test_order_heartbeat)
if(HAVE_NETWORK)
    target_compile_definitions(test_order_heartbeat PRIVATE CROWDINTEL_HAVE_NETWORK=1)
    target_link_libraries(test_order_heartbeat PRIVATE
        CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
endif()
set_target_properties(test_order_heartbeat PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The user-channel client lives on the WSS transport, so its tests are also
# network-build only (same convention as crowdintel-metadata).
if(HAVE_NETWORK)
    add_executable(test_user_ws ../tests/unit/test_user_ws.cpp)
    crowdintel_target(test_user_ws)
    target_link_libraries(test_user_ws PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    set_target_properties(test_user_ws PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# Read-only metadata resolver (Phase 1). Network-only: it must not exist as a
# live-looking stub in offline builds.
if(HAVE_NETWORK)
    add_executable(crowdintel-metadata src/metadata_tool.cpp)
    crowdintel_target(crowdintel-metadata)
    target_compile_definitions(crowdintel-metadata PRIVATE CROWDINTEL_HAVE_NETWORK=1)
    target_link_libraries(crowdintel-metadata PRIVATE
        CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
    set_target_properties(crowdintel-metadata PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    # Read-only account preflight (Phase 6). Same rule: a balances/allowances
    # check that cannot reach the venue must not exist as a stub.
    add_executable(crowdintel-preflight src/preflight_tool.cpp)
    crowdintel_target(crowdintel-preflight)
    target_compile_definitions(crowdintel-preflight PRIVATE CROWDINTEL_HAVE_NETWORK=1)
    target_link_libraries(crowdintel-preflight PRIVATE
        CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
    set_target_properties(crowdintel-preflight PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

add_executable(latency_bench ../tests/benchmarks/latency_bench.cpp)
crowdintel_target(latency_bench)
set_target_properties(latency_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(l2_backtester ../tests/replay/l2_backtester.cpp)
crowdintel_target(l2_backtester)
set_target_properties(l2_backtester PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

enable_testing()
add_test(NAME crypto_kat COMMAND $<TARGET_FILE:test_signer>)
add_test(NAME core_units COMMAND $<TARGET_FILE:test_core>)
add_test(NAME market_metadata COMMAND $<TARGET_FILE:test_market_metadata>)
add_test(NAME ledger COMMAND $<TARGET_FILE:test_ledger>)
add_test(NAME order_heartbeat COMMAND $<TARGET_FILE:test_order_heartbeat>)
add_test(NAME reconciliation COMMAND $<TARGET_FILE:test_reconciliation>)
add_test(NAME preflight COMMAND $<TARGET_FILE:test_preflight>)
add_test(NAME config COMMAND $<TARGET_FILE:test_config>)
# The operator-facing validation path: an empty environment must be enough for
# a replay configuration, and the process must exit before touching the network.
add_test(NAME config_check COMMAND env -i
    BOT_MODE=replay
    BOT_PRIVATE_KEY_HEX=23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b
    BOT_MARKET_SLUG=config-check
    $<TARGET_FILE:crowdintel_bot> --check-config)
if(HAVE_NETWORK)
    add_test(NAME user_ws COMMAND $<TARGET_FILE:test_user_ws>)
endif()
add_test(NAME backtester_smoke COMMAND $<TARGET_FILE:l2_backtester>
         ${CMAKE_CURRENT_SOURCE_DIR}/../tests/replay/sample_ticks.csv)
# latency_bench is a benchmark, not a correctness test; CI invokes it explicitly.
