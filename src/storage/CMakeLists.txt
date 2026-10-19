target_sources(${PROJECT_NAME}_lib PRIVATE
    redis/sentinel_monitor.cpp
    redis/redis_connection_pool.cpp
    redis/drain_worker.cpp
    redis/resilient_redis_producer.cpp
    redis/redis_pub_sub_worker.cpp
    redis/sharded_redis_manager.cpp
    redis/redis_track_repository.cpp
)
