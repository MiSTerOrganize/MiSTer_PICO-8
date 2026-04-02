//
//  compat_format.h — std::format polyfill for C++17
//
//  Provides a lightweight subset of std::format sufficient for zepto8.
//  Handles: {}, {:02x}, {:1x}, {:d}, {:c}, {:4x}, float formatting
//

#pragma once

#include <string>
#include <cstdio>
#include <cstdint>
#include <sstream>
#include <type_traits>

// Check if we already have std::format
#if __cplusplus >= 202002L
#  if __has_include(<format>)
#    include <format>
#    if defined(__cpp_lib_format)
#      define COMPAT_HAS_STD_FORMAT 1
#    endif
#  endif
#endif

#if !defined(COMPAT_HAS_STD_FORMAT)

namespace std
{

namespace detail_fmt
{
    // Format a single value with a printf-style spec (contents between : and })
    template<typename T>
    inline std::string format_one(const std::string &spec, T value)
    {
        char buf[128];

        if (spec.empty())
        {
            // Default formatting
            if constexpr (std::is_same_v<std::decay_t<T>, std::string>)
                return value;
            else if constexpr (std::is_same_v<std::decay_t<T>, const char*> ||
                               std::is_same_v<std::decay_t<T>, char*>)
                return std::string(value);
            else if constexpr (std::is_same_v<T, char>)
                return std::string(1, value);
            else if constexpr (std::is_floating_point_v<T>)
            {
                std::ostringstream oss;
                oss << static_cast<double>(value);
                return oss.str();
            }
            else if constexpr (std::is_integral_v<T>)
                return std::to_string(static_cast<long long>(value));
            else
            {
                std::ostringstream oss;
                oss << value;
                return oss.str();
            }
        }

        char type_char = spec.back();

        if (type_char == 'c')
        {
            if constexpr (std::is_integral_v<T>)
                return std::string(1, static_cast<char>(value));
            else
                return "?";
        }

        // Build a printf-compatible format string from the spec
        std::string pfmt = "%" + spec;

        if (type_char == 'x' || type_char == 'X')
        {
            if constexpr (std::is_integral_v<T>)
            {
                std::snprintf(buf, sizeof(buf), pfmt.c_str(),
                    static_cast<unsigned int>(
                        static_cast<typename std::make_unsigned<
                            std::conditional_t<std::is_signed_v<T>, T, T>
                        >::type>(value)));
                return buf;
            }
        }
        else if (type_char == 'd')
        {
            if constexpr (std::is_integral_v<T>)
            {
                std::snprintf(buf, sizeof(buf), pfmt.c_str(), static_cast<int>(value));
                return buf;
            }
        }
        else if (type_char == 'f' || type_char == 'e' || type_char == 'g')
        {
            if constexpr (std::is_arithmetic_v<T>)
            {
                std::snprintf(buf, sizeof(buf), pfmt.c_str(), static_cast<double>(value));
                return buf;
            }
        }

        // Fallback
        std::ostringstream oss;
        oss << value;
        return oss.str();
    }

    // Find next { placeholder, skip {{ escapes
    // Returns npos if not found, otherwise position after closing }
    // Fills spec with contents between : and }  (empty if just {})
    // Fills prefix_end with position of the opening {
    inline size_t find_next(const std::string &fmt, size_t pos,
                            size_t &prefix_end, std::string &spec, bool &is_escape)
    {
        while (pos < fmt.size())
        {
            size_t b = fmt.find('{', pos);
            if (b == std::string::npos) return std::string::npos;

            // {{ escape
            if (b + 1 < fmt.size() && fmt[b + 1] == '{')
            {
                prefix_end = b;
                is_escape = true;
                return b + 2;
            }

            size_t e = fmt.find('}', b + 1);
            if (e == std::string::npos) return std::string::npos;

            prefix_end = b;
            is_escape = false;
            if (b + 1 < e && fmt[b + 1] == ':')
                spec = fmt.substr(b + 2, e - b - 2);
            else
                spec.clear();
            return e + 1;
        }
        return std::string::npos;
    }

    // Base case: no more args
    inline std::string do_format(const std::string &fmt, size_t pos)
    {
        std::string result;
        for (size_t i = pos; i < fmt.size(); ++i)
        {
            if (i + 1 < fmt.size())
            {
                if (fmt[i] == '{' && fmt[i+1] == '{') { result += '{'; ++i; continue; }
                if (fmt[i] == '}' && fmt[i+1] == '}') { result += '}'; ++i; continue; }
            }
            result += fmt[i];
        }
        return result;
    }

    // Recursive case
    template<typename T, typename... Args>
    inline std::string do_format(const std::string &fmt, size_t pos, T value, Args... args)
    {
        size_t prefix_end;
        std::string spec;
        bool is_escape;

        size_t next = find_next(fmt, pos, prefix_end, spec, is_escape);
        if (next == std::string::npos)
            return do_format(fmt, pos);

        std::string result;
        // Add text before placeholder (with }} escape handling)
        for (size_t i = pos; i < prefix_end; ++i)
        {
            if (i + 1 < prefix_end && fmt[i] == '}' && fmt[i+1] == '}')
                { result += '}'; ++i; }
            else
                result += fmt[i];
        }

        if (is_escape)
        {
            result += '{';
            return result + do_format(fmt, next, value, args...);
        }

        result += format_one(spec, value);
        return result + do_format(fmt, next, args...);
    }

} // namespace detail_fmt

template<typename... Args>
inline std::string format(const std::string &fmt, Args... args)
{
    return detail_fmt::do_format(fmt, 0, args...);
}

template<typename... Args>
inline std::string format(const char *fmt, Args... args)
{
    return detail_fmt::do_format(std::string(fmt), 0, args...);
}

} // namespace std

#endif // !COMPAT_HAS_STD_FORMAT
