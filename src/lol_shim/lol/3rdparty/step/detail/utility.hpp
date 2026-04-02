// Andrew Naplavkov

#ifndef STEP_UTILITY_HPP
#define STEP_UTILITY_HPP

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace step {

/// @see https://en.cppreference.com/w/cpp/utility/variant/visit
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

template <class... Ts>
overloaded(Ts...)->overloaded<Ts...>;

/// @see https://en.cppreference.com/w/cpp/iterator/iter_t
template <class It>
using iter_value_t = typename std::iterator_traits<It>::value_type;

/// @see https://en.cppreference.com/w/cpp/ranges/iterator_t
template <class Rng>
using iterator_t = decltype(std::begin(std::declval<Rng&>()));

template <class Rng>
using range_value_t = iter_value_t<iterator_t<Rng>>;

template <class Compare>
class equivalence {
    Compare cmp_;

public:
    template <class T>
    constexpr bool operator()(const T& lhs, const T& rhs) const
    {
        return !cmp_(lhs, rhs) && !cmp_(rhs, lhs);
    }
};

template <class, class = std::void_t<>>
struct has_key_equal : std::false_type {
};

template <class T>
struct has_key_equal<T, std::void_t<typename T::key_equal>> : std::true_type {
};

template <class T>
struct key_equal {
    using type = typename T::key_equal;
};

template <class T>
struct key_equivalence {
    using type = equivalence<typename T::key_compare>;
};

template <class T>
using key_equal_or_equivalence_t =
    typename std::conditional_t<has_key_equal<T>::value,
                                key_equal<T>,
                                key_equivalence<T>>::type;

struct make_pair {
    template <class Lhs, class Rhs>
    constexpr auto operator()(Lhs&& lhs, Rhs&& rhs) const
    {
        return std::make_pair(std::forward<Lhs>(lhs), std::forward<Rhs>(rhs));
    }
};

struct make_reverse_pair {
    template <class Lhs, class Rhs>
    constexpr auto operator()(Lhs&& lhs, Rhs&& rhs) const
    {
        return std::make_pair(std::forward<Rhs>(rhs), std::forward<Lhs>(lhs));
    }
};

template <class T, size_t N>
class ring_table {
    std::array<std::vector<T>, N> rows_;

public:
    explicit ring_table(size_t cols)
    {
        for (auto& row : rows_)
            row.resize(cols);
    }

    auto& operator[](size_t row) { return rows_[row % N]; }
};

template <class T>
std::enable_if_t<std::is_unsigned_v<T>, T> flip(T n)
{
    return std::numeric_limits<T>::max() - n;
}

template <class T>
auto size(const std::pair<T, T>& pair)
{
    return pair.second - pair.first;
}

template <class T>
auto shifted_value_or(size_t shift, T default_value)
{
    return [=](const auto& rng, size_t pos) {
        pos += shift;
        return pos < std::size(rng) ? rng[pos] : default_value;
    };
}

template <class T, class... It>
void append(T& dest, std::pair<It, It>... src)
{
    using size_type = decltype(dest.size());
    dest.reserve(dest.size() + ((size_type)size(src) + ...));
    (std::copy(src.first, src.second, std::back_inserter(dest)), ...);
}

template <class Searcher, class... It>
auto find(Searcher searcher, std::pair<It, It>... rngs)
{
    auto count = (size(rngs) + ...);
    if (count < std::numeric_limits<int8_t>::max())
        return searcher.template find_with<uint8_t>(rngs...);
    else if (count < std::numeric_limits<int16_t>::max())
        return searcher.template find_with<uint16_t>(rngs...);
    else if (count < std::numeric_limits<int32_t>::max())
        return searcher.template find_with<uint32_t>(rngs...);
    else
        return searcher.template find_with<size_t>(rngs...);
}

}  // namespace step

#endif  // STEP_UTILITY_HPPs
