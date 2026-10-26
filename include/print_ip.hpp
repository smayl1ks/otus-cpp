#pragma once

#include <cstddef>
#include <iostream>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

/**
 * @file print_ip.hpp
 * @brief Реализация перегруженной функции print_ip для вывода условных IP-адресов с использованием механизма SFINAE.
 */

namespace detail
{

/**
 * @brief Метафункция для определения, является ли тип T стандартным контейнером.
 * 
 * Проверяет наличие типов value_type, iterator, а также методов begin() и end().
 */
template <typename T, typename = void>
struct is_container : std::false_type {};

template <typename T>
struct is_container<T, std::void_t<
    typename T::value_type,
    typename T::iterator,
    decltype(std::declval<T>().begin()),
    decltype(std::declval<T>().end())
>> : std::true_type {};

template <typename T>
inline constexpr bool is_container_v = is_container<T>::value;

/**
 * @brief Метафункция для определения, является ли тип T кортежем std::tuple.
 */
template <typename T>
struct is_tuple : std::false_type {};

template <typename... Ts>
struct is_tuple<std::tuple<Ts...>> : std::true_type {};

template <typename T>
inline constexpr bool is_tuple_v = is_tuple<T>::value;

/**
 * @brief Метафункция для проверки, что все типы в наборе совпадают с первым типом.
 */
template <typename... Ts>
struct all_same : std::true_type {};

template <typename T, typename... Ts>
struct all_same<T, Ts...> : std::conjunction<std::is_same<T, Ts>...> {};

template <typename T>
struct are_tuple_types_same;

template <typename... Ts>
struct are_tuple_types_same<std::tuple<Ts...>> : all_same<Ts...> {};

template <typename T>
inline constexpr bool are_tuple_types_same_v = are_tuple_types_same<T>::value;

/**
 * @brief Вспомогательная функция для поэлементного вывода std::tuple через разделитель точку.
 */
template <typename Tuple, std::size_t... Is>
void print_tuple_helper(const Tuple& t, std::index_sequence<Is...>, std::ostream& os)
{
    std::size_t index = 0;
    auto print_elem = [&](const auto& elem) {
        if (index++ > 0)
        {
            os << '.';
        }
        os << elem;
    };
    (print_elem(std::get<Is>(t)), ...);
}

} // namespace detail

/**
 * @brief Вывод IP-адреса, представленного произвольным целочисленным типом.
 * 
 * Значение выводится побайтово в беззнаковом виде, начиная со старшего байта.
 * Байты разделяются символом точки ('.').
 * 
 * @tparam T Произвольный целочисленный тип (std::is_integral_v<T> == true).
 * @param value Числовое значение IP-адреса.
 * @param os Поток вывода (по умолчанию std::cout).
 */
template <typename T>
std::enable_if_t<std::is_integral_v<T>>
print_ip(T value, std::ostream& os = std::cout)
{
    using UnsignedT = std::make_unsigned_t<T>;
    UnsignedT uval = static_cast<UnsignedT>(value);

    for (std::size_t i = sizeof(T); i > 0; --i)
    {
        auto byte = static_cast<unsigned char>((uval >> ((i - 1) * 8)) & 0xFF);
        os << static_cast<unsigned int>(byte);
        if (i > 1)
        {
            os << '.';
        }
    }
    os << '\n';
}

/**
 * @brief Вывод IP-адреса, представленного в виде строки.
 * 
 * Выводит содержимое строки как есть.
 * 
 * @tparam T Тип строки std::string.
 * @param value Строковое представление IP-адреса.
 * @param os Поток вывода (по умолчанию std::cout).
 */
template <typename T>
std::enable_if_t<std::is_same_v<T, std::string>>
print_ip(const T& value, std::ostream& os = std::cout)
{
    os << value << '\n';
}

/**
 * @brief Вывод IP-адреса, представленного стандартным контейнером (std::vector, std::list).
 * 
 * Поэлементно выводит элементы контейнера, разделяя их точкой ('.').
 * 
 * @tparam T Тип контейнера (за исключением std::string).
 * @param container Контейнер со значениями адреса.
 * @param os Поток вывода (по умолчанию std::cout).
 */
template <typename T>
std::enable_if_t<detail::is_container_v<T> && !std::is_same_v<T, std::string>>
print_ip(const T& container, std::ostream& os = std::cout)
{
    for (auto it = container.begin(); it != container.end(); ++it)
    {
        if (it != container.begin())
        {
            os << '.';
        }
        os << *it;
    }
    os << '\n';
}

/**
 * @brief Вывод IP-адреса, представленного кортежем std::tuple.
 * 
 * Поэлементно выводит элементы кортежа через точку.
 * Все типы элементов в кортеже обязаны быть одинаковыми, иначе произойдет ошибка компиляции.
 * 
 * @tparam T Тип std::tuple.
 * @param t Кортеж элементов.
 * @param os Поток вывода (по умолчанию std::cout).
 */
template <typename T>
std::enable_if_t<detail::is_tuple_v<T>>
print_ip(const T& t, std::ostream& os = std::cout)
{
    static_assert(detail::are_tuple_types_same_v<T>, "All types in std::tuple must be identical for print_ip!");
    detail::print_tuple_helper(t, std::make_index_sequence<std::tuple_size_v<T>>{}, os);
    os << '\n';
}
