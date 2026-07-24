cmake_minimum_required(VERSION 3.12)

file(READ "${CMAKE_SOURCE_DIR}/VERSION" PROJECT_VERSION_CONTENT)
string(STRIP "${PROJECT_VERSION_CONTENT}" PROJECT_VERSION)
project(wuwe VERSION ${PROJECT_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WUWE_ENABLE_ADDRESS_SANITIZER
    "Enable AddressSanitizer (and UndefinedBehaviorSanitizer where supported)"
    OFF)
option(WUWE_ENABLE_THREAD_SANITIZER
    "Enable ThreadSanitizer on supported Clang/GCC toolchains"
    OFF)

macro(wuwe_add_global_link_options)
    if(COMMAND add_link_options)
        add_link_options(${ARGN})
    else()
        foreach(wuwe_link_option IN LISTS ARGN)
            string(APPEND CMAKE_EXE_LINKER_FLAGS " ${wuwe_link_option}")
            string(APPEND CMAKE_SHARED_LINKER_FLAGS " ${wuwe_link_option}")
            string(APPEND CMAKE_MODULE_LINKER_FLAGS " ${wuwe_link_option}")
        endforeach()
    endif()
endmacro()

if(WUWE_ENABLE_ADDRESS_SANITIZER AND WUWE_ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR
        "WUWE_ENABLE_ADDRESS_SANITIZER and WUWE_ENABLE_THREAD_SANITIZER are mutually exclusive")
endif()

if(WUWE_ENABLE_ADDRESS_SANITIZER)
    if(MSVC)
        add_compile_options(/fsanitize=address)
        wuwe_add_global_link_options(/INCREMENTAL:NO)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        wuwe_add_global_link_options(-fsanitize=address,undefined)
    else()
        message(FATAL_ERROR
            "WUWE_ENABLE_ADDRESS_SANITIZER is not supported by ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

if(WUWE_ENABLE_THREAD_SANITIZER)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        wuwe_add_global_link_options(-fsanitize=thread)
    else()
        message(FATAL_ERROR
            "WUWE_ENABLE_THREAD_SANITIZER requires a supported Clang or GCC toolchain")
    endif()
endif()

if (MSVC)
    add_compile_options(/Zc:__cplusplus /permissive- /utf-8)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-pedantic)
endif()

add_subdirectory(src)
add_subdirectory(examples)

enable_testing()
add_subdirectory(tests)
