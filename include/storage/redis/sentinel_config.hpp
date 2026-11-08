#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace run::redis
{

struct SentinelNode
{
    std::string host{"127.0.0.1"};
    std::string port{"26379"};
};

struct SentinelConfig
{
    std::string master_prefix{"master"}; // nodes -> master-0, master-1, ...
    std::vector<SentinelNode> sentinel_nodes;
    // Used only when sentinel_nodes is empty: boost-redis then connects to
    // this address directly (sentinel discovery disabled)
    std::string direct_host{"127.0.0.1"};
    std::string direct_port{"6379"};
    std::string password;
    std::chrono::milliseconds connection_timeout{5000};
    std::size_t num_shards{1};
};

} // namespace run::redis