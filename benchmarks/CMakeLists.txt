# Gameplay benchmarks
add_executable(gameplay_benchmarks
    gameplay_benchmarks.cpp
)

target_link_libraries(gameplay_benchmarks PRIVATE
    ${PROJECT_NAME}_core
    benchmark::benchmark
    benchmark::benchmark_main
)

# Lets benchmarks load fixtures (e.g. the real GPX track) from the source tree
target_compile_definitions(gameplay_benchmarks PRIVATE
    RUN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)
