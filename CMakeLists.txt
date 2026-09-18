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
# Com -static, o linker nao aceita vincular um .so encontrado por find_library;
# forcamos a busca pelo arquivo estatico (.a) quando IOT_STATIC_ARM=ON.
if(IOT_STATIC_ARM)
    set(_gmp_lib_suffixes_backup ${CMAKE_FIND_LIBRARY_SUFFIXES})
    set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif()

find_path(GMP_INCLUDE_DIR gmp.h)
find_library(GMP_LIBRARY NAMES gmp)

if(IOT_STATIC_ARM)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ${_gmp_lib_suffixes_backup})
endif()

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

# ---------------------------------------------------------------------------
# Suite de benchmark refatorada para o ambiente FULL-SYSTEM (qemu-system-arm)
# EIXO 1: Validacao RFC + robustez | EIXO 2: escalabilidade | EIXO 3: custo
# amortizado/throughput | EIXO 4: memoria via getrusage/procfs.
# ---------------------------------------------------------------------------
add_executable(benchmark_suite_fullsystem
    benchmarks/benchmark_suite_fullsystem.cpp)

target_include_directories(benchmark_suite_fullsystem PRIVATE ${GMP_INCLUDE_DIR})
target_link_libraries(benchmark_suite_fullsystem PRIVATE ${GMP_LIBRARY})
target_compile_features(benchmark_suite_fullsystem PRIVATE cxx_std_17)
target_compile_options(benchmark_suite_fullsystem PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -O2>)
# GCC < 9 exige -lstdc++fs para <filesystem>; inofensivo em GCC >= 9 (biblioteca vazia/ja mesclada).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(benchmark_suite_fullsystem PRIVATE stdc++fs)
endif()

# Ligação estática: binário ARM auto-contido — necessário para copiar via scp
# e executar dentro da VM full-system sem depender de libgmp10 no guest.
if(IOT_STATIC_ARM)
    target_link_options(protocolo_hibrido PRIVATE -static)
    target_link_options(benchmark_suite   PRIVATE -static)
    target_link_options(demonstrador_defesa PRIVATE -static)
    target_link_options(benchmark_suite_fullsystem PRIVATE -static)
endif()
