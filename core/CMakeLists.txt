# CMakeLists.txt for Bot CrowdIntel Core (Hot Path)
# Optimized for Ultra-Low Latency with LTO, PGO, and CPU-specific instructions

cmake_minimum_required(VERSION 3.16)
project(CrowdIntelBot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Compiler Flags for Maximum Performance ---
# -O3: Aggressive optimization
# -march=native: Use all available CPU instructions (AVX2, etc.)
# -flto: Link Time Optimization (whole-program optimization)
# -fno-exceptions -fno-rtti: Remove C++ runtime overhead
add_compile_options(
    -O3
    -march=native
    -flto
    -fno-exceptions
    -fno-rtti
    -funroll-loops
    -DNDEBUG
)

# Linker Flags for LTO
add_link_options(-flto)

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
)

# --- Main Executable ---
add_executable(crowdintel_bot ${CORE_SOURCES})

# RPATH to find libraries
set_target_properties(crowdintel_bot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
