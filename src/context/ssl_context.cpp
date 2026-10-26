#include "context/ssl_context.hpp"
#include "config/config.hpp"
#include "logger/logger.hpp"

namespace run::context
{

constexpr long g_MaxSessionCache = 2048;
constexpr long g_SessionTimeoutSeconds = 300;

SSLContext::SSLContext()
{
    if (!SetCertificate())
    {
        throw std::runtime_error("Failed to setup SSL certificate");
    }
}

bool SSLContext::SetCertificate()
{
    sys::error_code error;

    m_ssl_context.set_options(net::ssl::context::default_workarounds | net::ssl::context::no_sslv2 |
                              net::ssl::context::no_sslv3 | net::ssl::context::no_tlsv1 |
                              net::ssl::context::no_tlsv1_1 | net::ssl::context::single_dh_use);

    auto native_ctx = m_ssl_context.native_handle();
    SSL_CTX_set_session_cache_mode(native_ctx, SSL_SESS_CACHE_SERVER);
    SSL_CTX_sess_set_cache_size(native_ctx, g_MaxSessionCache);
    SSL_CTX_set_options(native_ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
    SSL_CTX_set_mode(native_ctx, SSL_MODE_RELEASE_BUFFERS);
    SSL_CTX_set_timeout(native_ctx, g_SessionTimeoutSeconds);

    const auto cert = config::ServerConfig::Get().GetSSL().cert_file.string();
    m_ssl_context.use_certificate_chain_file(cert, error);

    if (error)
    {
        LOG_ERROR("Failed to load cert chain {}: {}", cert, error.message());
        return false;
    }

    const auto key = config::ServerConfig::Get().GetSSL().key_file.string();
    m_ssl_context.use_private_key_file(key, net::ssl::context::pem, error);

    if (error)
    {
        LOG_ERROR("Failed to load private key {}: {}", key, error.message());
        return false;
    }

    return true;
}

} // namespace run::context