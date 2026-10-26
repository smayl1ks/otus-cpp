#include "utils/crypto.hpp"

namespace run::utils::crypto
{

std::string HashPassword(std::string_view password)
{
    char hashed_password[SODIUM_MIN(crypto_pwhash_STRBYTES, 128)];

    if (crypto_pwhash_str(hashed_password, password.data(), password.size(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                          crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
    {
        throw std::runtime_error("Out of memory during password hashing");
    }

    return std::string(hashed_password);
}

bool VerifyPassword(std::string_view password, std::string_view password_hash)
{
    if (password_hash.empty())
    {
        return false;
    }

    return crypto_pwhash_str_verify(password_hash.data(), password.data(), password.size()) == 0;
}

std::string GenerateAccessToken(uint64_t user_id, std::string_view jwt_secret, std::chrono::seconds expires_in)
{
    using namespace std::chrono;

    return jwt::create<jwt::traits::nlohmann_json>()
        .set_subject(std::to_string(user_id))
        .set_issuer("run")
        .set_audience("api")
        .set_issued_at(system_clock::now())
        .set_expires_at(system_clock::now() + expires_in)
        .sign(jwt::algorithm::hs256{std::string(jwt_secret)});
}

std::optional<uint64_t> VerifyAccessToken(std::string_view token, std::string_view jwt_secret)
{
    try
    {
        auto decoded = jwt::decode<jwt::traits::nlohmann_json>(std::string(token));
        auto verifier = jwt::verify<jwt::traits::nlohmann_json>()
                            .allow_algorithm(jwt::algorithm::hs256{std::string(jwt_secret)})
                            .with_issuer("run")
                            .with_audience("api");

        verifier.verify(decoded);

        std::string sub = decoded.get_subject();
        return std::stoull(sub);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

} // namespace run::utils::crypto