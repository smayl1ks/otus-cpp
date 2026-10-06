cmake_minimum_required(VERSION 3.16)

project(outus-final-project VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/packages)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.hpp"
    @ONLY
)

include(cmake/packages.cmake)

# set(PROTO_FILES
#     proto/location.proto
#     proto/common.proto
#     proto/telemetry.proto
#     proto/events.proto
#     proto/envelope.proto
#     proto/gamemap.proto
#     proto/user.proto
# )

# protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

# add_library(${PROJECT_NAME}_lib STATIC ${PROTO_SRCS} ${PROTO_HDRS})

target_include_directories(${PROJECT_NAME}_lib PUBLIC
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(${PROJECT_NAME}_lib PUBLIC 
    ${PACKAGES_LINK_LIBRARIES} 
    protobuf::libprotobuf
)

target_compile_options(${PROJECT_NAME}_lib PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wno-maybe-uninitialized>
)

# target_precompile_headers(${PROJECT_NAME}_lib
#     PUBLIC 
#         include/common.hpp
# )

add_subdirectory(src)

add_executable(${PROJECT_NAME} src/main.cpp)

target_precompile_headers(${PROJECT_NAME}
    REUSE_FROM ${PROJECT_NAME}_lib
)

target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}_lib)

include(cmake/strip.cmake)

include(cmake/install.cmake)