cmake_minimum_required(VERSION 3.8)

project(dpdk_forward C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DPDK REQUIRED libdpdk)

if (DPDK_FOUND)
    message(STATUS "Found DPDK via pkg-config")
    add_library(dpdk INTERFACE)

    target_include_directories(dpdk INTERFACE ${DPDK_INCLUDE_DIRS})
    target_compile_options(dpdk INTERFACE ${DPDK_CFLAGS_OTHER})

    foreach(flag ${DPDK_LDFLAGS})
        target_link_libraries(dpdk INTERFACE "${flag}")
    endforeach()
else()
    message(FATAL_ERROR "DPDK not found")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-deprecated-declarations -Wall")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=znver4 -mtune=znver4")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

file(GLOB SRC_FILES src/*.c)

add_executable(dpdk_forward ${SRC_FILES})

target_link_libraries(dpdk_forward
PUBLIC
    dpdk -lpthread
)
