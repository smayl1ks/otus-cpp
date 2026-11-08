add_library(${PROJECT_NAME}_core STATIC)

add_subdirectory(config)
add_subdirectory(context)
add_subdirectory(controllers)
add_subdirectory(gameplay)
add_subdirectory(logger)
add_subdirectory(network)
add_subdirectory(repository)
add_subdirectory(service)
add_subdirectory(storage)
add_subdirectory(utils)

target_link_libraries(${PROJECT_NAME}_core PUBLIC ${PROJECT_NAME}_lib)

add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}_core)
set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME runner
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/strip.cmake)
