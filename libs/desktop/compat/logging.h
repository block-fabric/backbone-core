// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Compat: Bitcoin Core's logging macros, as the template's webui host files use them, written to the
// engine's log (bt/log.h). Categories are accepted and not filtered.
#pragma once

#include <bt/log.h>
#include <tinyformat.h>

#include <cstdint>
#include <string>

namespace BCLog {
enum LogFlags : uint64_t { NONE = 0, NET = 1, RPC = 2, HTTP = 4, WALLET = 8, ALL = ~uint64_t{0} };
} // namespace BCLog

namespace bt_compat {
inline void Log(::bt::log::Level level, std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    ::bt::log::write(level, "%s", text.c_str());
}
} // namespace bt_compat

#define LogInfo(...) ::bt_compat::Log(::bt::log::Level::Info, strprintf(__VA_ARGS__))
#define LogPrintf(...) ::bt_compat::Log(::bt::log::Level::Info, strprintf(__VA_ARGS__))
#define LogWarning(...) ::bt_compat::Log(::bt::log::Level::Warn, strprintf(__VA_ARGS__))
#define LogError(...) ::bt_compat::Log(::bt::log::Level::Error, strprintf(__VA_ARGS__))
#define LogDebug(category, ...) ::bt_compat::Log(::bt::log::Level::Debug, strprintf(__VA_ARGS__))
#define LogTrace(category, ...) ::bt_compat::Log(::bt::log::Level::Trace, strprintf(__VA_ARGS__))
