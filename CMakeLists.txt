cmake_minimum_required(VERSION 4.1)
project (nzbweaver C)

set (BUILD_NATIVE ON CACHE BOOL "Force Native Build" FORCE)
set (DISABLE_AVX256 OFF CACHE BOOL "Enable AVX256" FORCE)
set (DISABLE_CRCUTIL ON CACHE BOOL "Enable crcutil lib." FORCE)
set (DISABLE_ENCODE ON CACHE BOOL "Disable Encoding Functions" FORCE)
set (DISABLE_DECODE OFF CACHE BOOL "Enable Decoding Functions" FORCE)
set (DISABLE_CRC OFF CACHE BOOL "Enable CRC32" FORCE)

add_subdirectory(rapidyenc)
add_subdirectory(src)
