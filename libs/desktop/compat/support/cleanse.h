// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Compat: Bitcoin Core's memory_cleanse (support/cleanse.h), on libsodium's sodium_memzero.
#pragma once

#include <cstddef>

void memory_cleanse(void* ptr, size_t len);
