cmake_minimum_required(VERSION 3.20)
project(wuwe_source_subdirectory_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(WUWE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WUWE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WUWE_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(WUWE_FETCH_CPR ON CACHE BOOL "" FORCE)
set(WUWE_INSTALL_BUNDLED_RUNTIME OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../.." wuwe-source)

if(NOT CMAKE_CXX_STANDARD EQUAL 17)
    message(FATAL_ERROR "Wuwe changed the consumer's global C++ standard")
endif()

add_executable(wuwe_source_subdirectory_smoke main.cpp)
target_link_libraries(wuwe_source_subdirectory_smoke PRIVATE wuwe)
