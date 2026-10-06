add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND $<$<CONFIG:release>:${CMAKE_STRIP}>
    $<TARGET_FILE:${PROJECT_NAME}>
)