set(SERVICE_NAME ${PROJECT_NAME})

set(SERVICE_DESCRIPTION    "Outus final project")
set(SYSTEMD_SERVICE_FOLDER "/lib/systemd/system")
set(SERVICE_FOLDER         "/opt/${SERVICE_NAME}")

configure_file(
    ${PROJECT_SOURCE_DIR}/service/${SERVICE_NAME}.service.in
    ${PROJECT_BINARY_DIR}/service/${SERVICE_NAME}.service @ONLY)

configure_file(
    ${PROJECT_SOURCE_DIR}/service/postinst.in
    ${PROJECT_BINARY_DIR}/service/postinst @ONLY)

install(FILES ${PROJECT_BINARY_DIR}/service/${SERVICE_NAME}.service
        DESTINATION ${SYSTEMD_SERVICE_FOLDER})

install(TARGETS ${PROJECT_NAME}
        DESTINATION ${SERVICE_FOLDER})

include(${PROJECT_SOURCE_DIR}/cmake/cpack.cmake)
MakePackage(${PROJECT_NAME} ${PROJECT_BINARY_DIR}/service/postinst)