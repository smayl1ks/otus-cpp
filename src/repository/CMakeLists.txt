target_sources(${PROJECT_NAME}_core PRIVATE
    connection_pool.cpp
    postgres_hex_ownership_source.cpp
    postgres_run_history_repository.cpp
    postgres_user_repository.cpp
    redis_active_run_store.cpp
    redis_hex_map_cache.cpp
)
