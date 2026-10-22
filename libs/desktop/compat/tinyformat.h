// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Compat: strprintf / tfm::format as Bitcoin Core's tinyformat offers them, for the template's webui host
// files built outside Core. printf-style specifications with tinyformat's semantics: every argument is
// written with operator<< under the flags of its specification (%s of a number is the number, %d of a
// string is the string), length modifiers (l, ll, z, h, j, t) are ignored, %% is a percent sign.
#pragma once

#include <cstddef>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace tinyformat {

class format_error : public std::runtime_error
{
public:
    explicit format_error(const std::string& what) : std::runtime_error(what) {}
};

namespace detail {

struct Spec {
    bool left{false}, zero{false}, plus{false}, space{false}, alt{false};
    int width{-1}, precision{-1};
    char conv{'s'};
};

/** Reads one specification after '%'; returns where it ends. */
inline size_t ParseSpec(std::string_view fmt, size_t i, Spec& spec)
{
    for (; i < fmt.size(); ++i) {
        const char c{fmt[i]};
        if (c == '-') spec.left = true;
        else if (c == '0') spec.zero = true;
        else if (c == '+') spec.plus = true;
        else if (c == ' ') spec.space = true;
        else if (c == '#') spec.alt = true;
        else break;
    }
    if (i < fmt.size() && fmt[i] >= '1' && fmt[i] <= '9') {
        spec.width = 0;
        while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') spec.width = spec.width * 10 + (fmt[i++] - '0');
    }
    if (i < fmt.size() && fmt[i] == '.') {
        ++i;
        spec.precision = 0;
        while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') spec.precision = spec.precision * 10 + (fmt[i++] - '0');
    }
    while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == 'z' || fmt[i] == 'j' || fmt[i] == 't' || fmt[i] == 'L' || fmt[i] == 'q')) ++i;
    if (i >= fmt.size()) throw format_error("tinyformat: the format string ends inside a specification");
    spec.conv = fmt[i];
    return i + 1;
}

template <typename T>
void WriteOne(std::ostream& out, const Spec& spec, const T& value)
{
    std::ostringstream s;
    s.copyfmt(out);
    if (spec.precision >= 0) s << std::setprecision(spec.precision);
    switch (spec.conv) {
    case 'x': s << std::hex; break;
    case 'X': s << std::hex << std::uppercase; break;
    case 'o': s << std::oct; break;
    case 'f': case 'F': s << std::fixed; break;
    case 'e': case 'E': s << std::scientific; break;
    default: break;
    }
    if (spec.alt) s << std::showbase;
    if (spec.plus) s << std::showpos;
    if constexpr (std::is_same_v<std::decay_t<T>, char> || std::is_same_v<std::decay_t<T>, signed char> || std::is_same_v<std::decay_t<T>, unsigned char>) {
        if (spec.conv == 'c' || spec.conv == 's') s << static_cast<char>(value);
        else s << static_cast<int>(value);
    } else if constexpr (std::is_same_v<std::decay_t<T>, bool>) {
        if (spec.conv == 's') s << (value ? "true" : "false");
        else s << static_cast<int>(value);
    } else {
        s << value;
    }
    std::string text{s.str()};
    if (spec.conv == 's' && spec.precision >= 0 && text.size() > static_cast<size_t>(spec.precision)) text.resize(spec.precision);
    if (spec.width > 0 && text.size() < static_cast<size_t>(spec.width)) {
        const size_t pad{spec.width - text.size()};
        if (spec.left) {
            text.append(pad, ' ');
        } else if (spec.zero && spec.conv != 's' && spec.conv != 'c') {
            const size_t sign{(!text.empty() && (text[0] == '-' || text[0] == '+')) ? size_t{1} : size_t{0}};
            text.insert(sign, pad, '0');
        } else {
            text.insert(0, pad, ' ');
        }
    }
    out << text;
}

/** Writes the literal text up to the next specification; returns where it is (or npos). */
inline size_t Literal(std::ostream& out, std::string_view fmt, size_t i)
{
    while (i < fmt.size()) {
        const size_t pct{fmt.find('%', i)};
        if (pct == std::string_view::npos) {
            out << fmt.substr(i);
            return std::string_view::npos;
        }
        out << fmt.substr(i, pct - i);
        if (pct + 1 < fmt.size() && fmt[pct + 1] == '%') {
            out << '%';
            i = pct + 2;
            continue;
        }
        return pct;
    }
    return std::string_view::npos;
}

inline void FormatImpl(std::ostream& out, std::string_view fmt, size_t i)
{
    i = Literal(out, fmt, i);
    if (i != std::string_view::npos) throw format_error("tinyformat: more specifications than arguments");
}

template <typename T, typename... Rest>
void FormatImpl(std::ostream& out, std::string_view fmt, size_t i, const T& first, const Rest&... rest)
{
    i = Literal(out, fmt, i);
    if (i == std::string_view::npos) throw format_error("tinyformat: more arguments than specifications");
    Spec spec;
    i = ParseSpec(fmt, i + 1, spec);
    WriteOne(out, spec, first);
    FormatImpl(out, fmt, i, rest...);
}

} // namespace detail

template <typename... Args>
void format(std::ostream& out, std::string_view fmt, const Args&... args)
{
    detail::FormatImpl(out, fmt, 0, args...);
}

template <typename... Args>
std::string format(std::string_view fmt, const Args&... args)
{
    std::ostringstream out;
    format(out, fmt, args...);
    return out.str();
}

} // namespace tinyformat

namespace tfm = tinyformat;

template <typename... Args>
std::string strprintf(std::string_view fmt, const Args&... args)
{
    return tfm::format(fmt, args...);
}
