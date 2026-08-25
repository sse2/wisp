cmake_minimum_required(VERSION 3.16)
project(wisp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WISP_ASAN "Build with AddressSanitizer" OFF)

if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        add_compile_options(-fms-runtime-lib=static)
        add_link_options(-fms-runtime-lib=static)
    endif()
endif()

set(CMAKE_THREAD_PREFER_PTHREAD ON)
find_package(Threads REQUIRED)

add_library(wisp_warnings INTERFACE)
if(MSVC)
    target_compile_options(wisp_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(wisp_warnings INTERFACE
        -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wno-unused-parameter)
endif()

if(WIN32)
    target_compile_definitions(wisp_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
endif()

if(WISP_ASAN AND NOT MSVC)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

add_subdirectory(thirdparty)
add_subdirectory(src)
