#pragma once

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <sodium.h>
#include <string>
#include <string_view>

namespace run::utils::crypto
{

std::string HashPassword(std::string_view password);

bool VerifyPassword(std::string_view password, std::string_view password_hash);

std::string GenerateAccessToken(uint64_t user_id, std::string_view jwt_secret,
                                std::chrono::seconds expires_in = std::chrono::hours(24 * 30));

std::optional<uint64_t> VerifyAccessToken(std::string_view token, std::string_view jwt_secret);

inline std::string GenerateRefreshToken()
{
    boost::uuids::random_generator gen;
    return boost::uuids::to_string(gen());
}

std::string HashSha256(std::string_view data);

} // namespace run::utils::crypto