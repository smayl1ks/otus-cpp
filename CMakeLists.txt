cmake_minimum_required(VERSION 3.12)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

set(PATCH_VERSION "1" CACHE STRING "Patch version")
set(PROJECT_VERSION 0.0.${PATCH_VERSION})

project(allocator VERSION ${PROJECT_VERSION})

option(WITH_BOOST_TEST "Whether to build Boost test" ON)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.h"
    @ONLY
)

add_executable(allocator src/main.cpp)

set_target_properties(allocator PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(allocator PRIVATE 
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(WITH_BOOST_TEST)
    find_package(Boost COMPONENTS unit_test_framework REQUIRED)
    
    add_executable(test_version tests/test_version.cpp)

    set_target_properties(test_version PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )

    target_compile_definitions(test_version PRIVATE BOOST_TEST_DYN_LINK)
    target_include_directories(test_version PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

    target_link_libraries(test_version PRIVATE
        Boost::unit_test_framework
    )
endif()

if (MSVC)
    target_compile_options(allocator PRIVATE /W4)
    if(WITH_BOOST_TEST)
        target_compile_options(test_version PRIVATE /W4)
    endif()
else ()
    target_compile_options(allocator PRIVATE -Wall -Wextra -pedantic -Werror)
    if(WITH_BOOST_TEST)
        target_compile_options(test_version PRIVATE -Wall -Wextra -pedantic -Werror)
    endif()
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

if(WITH_BOOST_TEST)
    enable_testing()
    add_test(NAME test_version COMMAND test_version)
endif()