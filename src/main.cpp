#include <iostream>
#include <map>

#include "allocator.hpp"
#include "container.hpp"

unsigned factorial(unsigned n)
{
    if (n <= 1)
    {
        return 1;
    }
    return n * factorial(n - 1);
}

int main()
{
    std::map<int, int> m1;
    for (int i = 0; i < 10; ++i)
    {
        m1[i] = factorial(i);
    }

    std::map<int, int, std::less<int>, CustomAllocator<std::pair<const int, int>, 10>> m2;
    for (int i = 0; i < 10; ++i)
    {
        m2[i] = factorial(i);
    }

    for (const auto& [k, v] : m1)
    {
        std::cout << k << ' ' << v << '\n';
    }

    std::cout << '\n';

    for (const auto& [k, v] : m2)
    {
        std::cout << k << ' ' << v << '\n';
    }

    std::cout << '\n';

    CustomContainer<int> c1;
    for (int i = 0; i < 10; ++i)
    {
        c1.push(i);
    }

    CustomContainer<int, CustomAllocator<int, 10>> c2;
    for (int i = 0; i < 10; ++i)
    {
        c2.push(i);
    }

    for (auto it = c1.begin(); it != c1.end(); ++it)
    {
        std::cout << *it << ' ';
    }   
    std::cout << '\n';

    for (auto it = c2.begin(); it != c2.end(); ++it)
    {
        std::cout << *it << ' ';
    }
    
    std::cout << '\n';

    return 0;
}
