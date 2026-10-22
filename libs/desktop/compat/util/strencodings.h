// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Compat: the parts of Bitcoin Core's util/strencodings.h the template's webui host files use.
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

std::string HexStr(std::span<const unsigned char> s);
inline std::string HexStr(std::span<const std::byte> s) { return HexStr(std::span<const unsigned char>{reinterpret_cast<const unsigned char*>(s.data()), s.size()}); }
std::string EncodeBase64(std::span<const unsigned char> input);
inline std::string EncodeBase64(std::string_view str) { return EncodeBase64(std::span<const unsigned char>{reinterpret_cast<const unsigned char*>(str.data()), str.size()}); }
std::optional<std::vector<unsigned char>> DecodeBase64(std::string_view str);
std::string ToLower(std::string_view str);
std::string ToUpper(std::string_view str);
bool IsHex(std::string_view str);

template <typename T>
std::optional<T> ToIntegral(std::string_view str)
{
    static_assert(std::is_integral_v<T>);
    T result;
    const auto [ptr, ec]{std::from_chars(str.data(), str.data() + str.size(), result)};
    if (ptr != str.data() + str.size() || ec != std::errc{}) return std::nullopt;
    return result;
}
