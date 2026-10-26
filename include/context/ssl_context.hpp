#pragma once

#include "common.hpp"

namespace run::context
{

class SSLContext
{
private:
    ssl::context m_ssl_context{ssl::context::tlsv12_server};

private:
    bool SetCertificate();

public:
    explicit SSLContext();

    SSLContext(const SSLContext&) = delete;
    SSLContext& operator=(const SSLContext&) = delete;

    ssl::context& GetSSLContext() noexcept
    {
        return m_ssl_context;
    }
};

} // namespace run::context