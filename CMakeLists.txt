cmake_minimum_required(VERSION 3.12)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

set(PATCH_VERSION "1" CACHE STRING "Patch version")
set(PROJECT_VERSION 0.0.${PATCH_VERSION})

project(allocator VERSION ${PROJECT_VERSION})

add_executable(allocator src/main.cpp)

set_target_properties(allocator PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(allocator PRIVATE 
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if (MSVC)
    target_compile_options(allocator PRIVATE /W4)
else ()
    target_compile_options(allocator PRIVATE -Wall -Wextra -pedantic -Werror)
endif()

install(TARGETS allocator RUNTIME DESTINATION bin)

# Настройки CPack
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "allocator")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_RELEASE "1")
set(CPACK_PACKAGE_CONTACT "example@example.com")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "example@example.com")

include(CPack)