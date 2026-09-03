#include "ip_filter.hpp"

#include <iostream>

int main(int argc, char const *argv[])
{
    (void)argc;
    (void)argv;

    try
    {
        filter::IPPool ip_pool;
        for(std::string line; std::getline(std::cin, line);)
        {
            if (line.empty())
            {
                continue;
            }

            filter::IP ip;

            if (std::sscanf(line.c_str(), "%d.%d.%d.%d", &ip[0], &ip[1], &ip[2], &ip[3]) == 4)
            {
                ip_pool.push_back(ip);
            }
        }

        filter::reverse_lexicographically_sort(ip_pool);

        filter::print_ip_pool(ip_pool);

        filter::print_ip_pool(filter::filter(ip_pool, 1));

        // TODO filter by first and second bytes and output
        // ip = filter(46, 70)

        filter::print_ip_pool(filter::filter(ip_pool, 46, 70));

        // TODO filter by any byte and output
        // ip = filter_any(46)

        filter::print_ip_pool(filter::filter_any(ip_pool, 46));

    }
    catch(const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
    }

    return 0;
}