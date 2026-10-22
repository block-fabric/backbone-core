// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Compat: Bitcoin Core's fs namespace (util/fs.h) as the template's webui host files use it: std::filesystem
// with UTF-8 conversions (PathToString, PathFromString).
#pragma once

#include <filesystem>
#include <string>

namespace fs {
using namespace std::filesystem;

inline std::string PathToString(const path& p)
{
#ifdef _WIN32
    const auto u8{p.u8string()};
    return std::string(u8.begin(), u8.end());
#else
    return p.string();
#endif
}

inline path PathFromString(const std::string& s)
{
#ifdef _WIN32
    return path(std::u8string(s.begin(), s.end()));
#else
    return path(s);
#endif
}
} // namespace fs
