cmake_minimum_required(VERSION 3.22.1)
project(RiMU2000Android LANGUAGES CXX)

set_property(GLOBAL PROPERTY JOB_POOLS native_compile=2)
set(CMAKE_JOB_POOL_COMPILE native_compile)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "RiMU2000 currently targets Android arm64-v8a")
endif()

get_filename_component(SMU_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
# Reuse the upstream source list. Desktop executables remain excluded.
add_subdirectory("${SMU_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/upstream" EXCLUDE_FROM_ALL)
add_library(rimu2000 SHARED native_engine.cpp)
target_compile_features(rimu2000 PRIVATE cxx_std_20)
target_compile_options(rimu2000 PRIVATE -O3 -Wall -Wextra -Wformat-security)
target_compile_options(smu2000_core PRIVATE -O3)
# Keep normal IEEE arithmetic: no fast-math or alterations to the sound engine.
target_link_options(rimu2000 PRIVATE "-Wl,-z,max-page-size=16384")
target_link_libraries(rimu2000 PRIVATE smu2000_core aaudio android log)
