// Andrew Naplavkov

#ifndef STEP_SUFFIX_ARRAY_HPP
#define STEP_SUFFIX_ARRAY_HPP

#include "detail/utility.hpp"

namespace step {

/// Manber's algorithm for constructing suffix array.

/// Time complexity O(N*log(N)*log(N)), space complexity O(N), where:
/// N - text length.
/// @param T - type of the characters;
/// @param Size - to specify the maximum number / offset of characters;
/// @param Compare - to determine the order of characters.
/// @see https://en.wikipedia.org/wiki/Suffix_array
template <class T = char, class Size = size_t, class Compare = std::less<>>
class suffix_array {
public:
    using value_type = T;
    using size_type = Size;

    auto data() const { return str_.data(); }
    Size size() const { return (Size)str_.size(); }

    /// Return offset of the n-th suffix in lexicographical order
    Size nth_element(Size nth) const { return idx_[nth]; }

    template <class InputIt>
    suffix_array(InputIt first, InputIt last)
        : suffix_array(std::vector<T>(first, last))
    {
    }

    template <class InputRng>
    explicit suffix_array(const InputRng& rng)
        : suffix_array(std::begin(rng), std::end(rng))
    {
    }

    explicit suffix_array(std::vector<T>&& str)
        : str_(std::move(str)), idx_(size())
    {
        auto generator = [i = Size{}]() mutable { return suffix{i++,{}}; };
        auto pos = [](auto& suf) { return suf.pos; };
        auto val = [&](auto& suf) { return str_[suf.pos]; };
        auto by_rank = [](auto& l, auto& r) { return l.rank < r.rank; };
        auto by_val = [&](auto& l, auto& r) { return cmp_(val(l), val(r)); };

        std::vector<suffix> sufs(size());
        std::generate(sufs.begin(), sufs.end(), generator);
        std::sort(sufs.begin(), sufs.end(), by_val);
        fill_first_rank(sufs, by_val);
        for (Size shift = 1; !sorted(sufs); shift *= 2) {
            fill_second_rank(sufs, shift);
            std::sort(sufs.begin(), sufs.end(), by_rank);
            fill_first_rank(sufs, by_rank);
        }
        std::transform(sufs.begin(), sufs.end(), idx_.begin(), pos);
    }

    /// Find all occurrences of the substring.

    /// Time complexity O(M*log(N)), where:
    /// M - substring length, N - text length.
    /// @return pair of offset iterators.
    template <class InputIt>
    auto find_all(InputIt first, InputIt last) const
    {
        auto result = std::make_pair(idx_.begin(), idx_.end());
        std::for_each(first, last, [&, i = Size{}](T val) mutable {
            auto at = shifted_value_or(i++, val);
            result = std::equal_range(
                result.first, result.second, size(), [&](Size l, Size r) {
                    return cmp_(at(str_, l), at(str_, r));
                });
        });
        return result;
    }

    template <class InputRng>
    auto find_all(const InputRng& rng) const
    {
        return find_all(std::begin(rng), std::end(rng));
    }

    /// Find offset of the substring
    template <class InputIt>
    Size find(InputIt first, InputIt last) const
    {
        auto rng = find_all(first, last);
        return step::size(rng) ? *rng.first : size();
    }

    template <class InputRng>
    Size find(const InputRng& rng) const
    {
        return find(std::begin(rng), std::end(rng));
    }

    /// Kasai's algorithm for constructing longest common prefix array.

    /// Time and space complexity O(N), where: N - text length.
    /// @see https://en.wikipedia.org/wiki/LCP_array
    template <class RandomIt>
    void longest_common_prefix_array(RandomIt result) const
    {
        std::vector<Size> inverse(size());
        for (Size i = 0; i < size(); ++i)
            inverse[idx_[i]] = i;
        for (Size pos = 0, lcp = 0; pos < size(); ++pos) {
            Size cur = inverse[pos];
            Size next = cur + 1;
            if (next < size()) {
                auto diff = std::mismatch(str_.begin() + pos + lcp,
                                          str_.end(),
                                          str_.begin() + idx_[next] + lcp,
                                          str_.end(),
                                          eq_);
                lcp = (Size)std::distance(str_.begin() + pos, diff.first);
            }
            else
                lcp = 0;
            result[cur] = lcp;
            if (lcp)
                --lcp;
        }
    }

private:
    inline static const auto cmp_ = Compare{};
    inline static const auto eq_ = equivalence<Compare>{};

    std::vector<T> str_;
    std::vector<Size> idx_;

    struct suffix {
        Size pos;
        std::pair<Size, Size> rank;
    };

    template <class Cmp>
    static void fill_first_rank(std::vector<suffix>& sufs, Cmp cmp)
    {
        Size uniq = 1;
        for (size_t i = 1; i < sufs.size(); ++i) {
            bool less = cmp(sufs[i - 1], sufs[i]);
            sufs[i - 1].rank.first = uniq;
            if (less)
                ++uniq;
        }
        if (!sufs.empty())
            sufs.back().rank.first = uniq;
    }

    static void fill_second_rank(std::vector<suffix>& sufs, Size shift)
    {
        std::vector<Size> ranks(sufs.size());
        for (auto& suf : sufs)
            ranks[suf.pos] = suf.rank.first;
        auto at = shifted_value_or(shift, Size{});
        for (auto& suf : sufs)
            suf.rank.second = at(ranks, suf.pos);
    }

    static bool sorted(const std::vector<suffix>& sufs)
    {
        return sufs.empty() || sufs.back().rank.first == (Size)sufs.size();
    }
};

template <class InputIt>
suffix_array(InputIt, InputIt)->suffix_array<iter_value_t<InputIt>>;

template <class InputRng>
suffix_array(InputRng)->suffix_array<range_value_t<InputRng>>;

}  // namespace step

#endif  // STEP_SUFFIX_ARRAY_HPP
