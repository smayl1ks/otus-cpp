#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <algorithm>

using IP = std::array<int, 4>;
using IPPool = std::vector<IP>;

void print_ip_pool(const IPPool& ip_pool)
{
    for (const auto& ip : ip_pool)
    {
        for (size_t i = 0; i < ip.size(); ++i)
        {
            if (i > 0) std::cout << '.';
            std::cout << ip[i];
        }
        std::cout << '\n';
    }
}

void reverse_lexicographically_sort(IPPool& ip_pool)
{
    std::sort(ip_pool.begin(), ip_pool.end(), std::greater<IP>());
}

template<typename... Args>
IPPool filter(const IPPool& ip_pool, Args... args)
{
    IPPool filtered_ip_pool;
    if (sizeof...(args) == 0)
    {
        return ip_pool;
    }

    for (const auto& ip : ip_pool)
    {
        std::array<int, sizeof...(args)> targets { static_cast<int>(args)... };
        
        bool match = true;

        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (ip[i] != targets[i])
            {
                match = false;
                break;
            }
        }

        if (match)
        {
            filtered_ip_pool.push_back(ip);
        }
    }

    return filtered_ip_pool;
}

template<typename... Args>
IPPool filter_any(const IPPool& ip_pool, Args... args)
{
    IPPool filtered_ip_pool;

    if (sizeof...(args) == 0)
    {
        return filtered_ip_pool;
    }

    for (const auto& ip : ip_pool)
    {
        for (const int byte : ip)
        {
            if (((byte == args) || ...))
            {
                filtered_ip_pool.push_back(ip);
                break;
            }
        }
    }

    return filtered_ip_pool;
}

int main(int argc, char const *argv[])
{
    (void)argc;
    (void)argv;

    try
    {
        IPPool ip_pool;
        for(std::string line; std::getline(std::cin, line);)
        {
            if (line.empty())
            {
                continue;
            }

            std::array<int, 4> ip;

            if (std::sscanf(line.c_str(), "%d.%d.%d.%d", &ip[0], &ip[1], &ip[2], &ip[3]) == 4)
            {
                ip_pool.push_back(ip);
            }
        }

        reverse_lexicographically_sort(ip_pool);

        print_ip_pool(ip_pool);

        print_ip_pool(filter(ip_pool, 1));

        // TODO filter by first and second bytes and output
        // ip = filter(46, 70)

        print_ip_pool(filter(ip_pool, 46, 70));

        // TODO filter by any byte and output
        // ip = filter_any(46)

        print_ip_pool(filter_any(ip_pool, 46));

    }
    catch(const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
    }

    return 0;
}
