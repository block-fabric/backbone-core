// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Compat: util::ThreadRename (Bitcoin Core util/threadnames.h).
#pragma once

#include <string>

namespace util {
void ThreadRename(const std::string& name);
} // namespace util
