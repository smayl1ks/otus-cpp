find_package(Boost REQUIRED CONFIG COMPONENTS system container program_options redis uuid)

find_package(OpenSSL REQUIRED)
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(h3 CONFIG REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

set(PACKAGES_LINK_LIBRARIES
    Boost::program_options
    Boost::redis
    Boost::system
    Boost::container
    Boost::uuid
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    GTest::gtest
    spdlog::spdlog_header_only
    h3::h3
    jwt-cpp::jwt-cpp
    protobuf::libprotobuf
    libpqxx::pqxx
    unofficial-sodium::sodium
    nlohmann_json::nlohmann_json
)

set(PACKAGES_INCLUDE_DIRECTORIES
        ${Boost_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR}
)