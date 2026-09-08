cmake_minimum_required(VERSION 3.21)

project(Hybrid_Lightweight_ECC_AEAD_IoT
    VERSION 2.1.0
    DESCRIPTION "Hybrid Lightweight ECC and AEAD Encryption Protocol for IoT"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(IOT_STATIC_ARM "Link statically (ARM QEMU — avoids QEMU_LD_PREFIX)" OFF)

# ---------------------------------------------------------------------------
# GMP
# ---------------------------------------------------------------------------
find_path(GMP_INCLUDE_DIR gmp.h)
find_library(GMP_LIBRARY NAMES gmp)
if(NOT GMP_INCLUDE_DIR OR NOT GMP_LIBRARY)
    message(FATAL_ERROR
        "GMP nao encontrado. Instale: sudo apt install libgmp-dev\n"
        "Para ARM: sudo apt install libgmp-dev:armhf  (cross-compilation)")
endif()

# ---------------------------------------------------------------------------
# Executável principal: protocolo híbrido + benchmark embutido
# ---------------------------------------------------------------------------
add_executable(protocolo_hibrido
    src/ECDSA_ECDH_C25519.cpp)

target_include_directories(protocolo_hibrido PRIVATE ${GMP_INCLUDE_DIR})
target_link_libraries(protocolo_hibrido PRIVATE ${GMP_LIBRARY})
target_compile_features(protocolo_hibrido PRIVATE cxx_std_17)
target_compile_options(protocolo_hibrido PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -O2>)

# ---------------------------------------------------------------------------
# Suite de benchmark multi-nível (80/112/128/192/224/256 bits)
# ---------------------------------------------------------------------------
add_executable(benchmark_suite
    benchmarks/benchmark_suite.cpp)

target_include_directories(benchmark_suite PRIVATE ${GMP_INCLUDE_DIR})
target_link_libraries(benchmark_suite PRIVATE ${GMP_LIBRARY})
target_compile_features(benchmark_suite PRIVATE cxx_std_17)
target_compile_options(benchmark_suite PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -O2>)

# Demonstrador interativo para a defesa, organizado por simulacao.
add_executable(demonstrador_defesa
    defesa/demonstrador_defesa.cpp)

target_include_directories(demonstrador_defesa PRIVATE ${GMP_INCLUDE_DIR})
target_link_libraries(demonstrador_defesa PRIVATE ${GMP_LIBRARY})
target_compile_features(demonstrador_defesa PRIVATE cxx_std_17)
target_compile_options(demonstrador_defesa PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -O2>)

# Ligação estática: binário ARM auto-contido — roda com "qemu-arm ./binary" sem QEMU_LD_PREFIX
if(IOT_STATIC_ARM)
    target_link_options(protocolo_hibrido PRIVATE -static)
    target_link_options(benchmark_suite   PRIVATE -static)
    target_link_options(demonstrador_defesa PRIVATE -static)
endif()
