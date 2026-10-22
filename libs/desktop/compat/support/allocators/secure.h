// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Compat: Bitcoin Core's secure_allocator and SecureString (support/allocators/secure.h): memory wiped when
// freed (memory_cleanse). Not locked into RAM here (Core's LockedPool): the app holds a passphrase only for the
// moment it forwards it to its node.
#pragma once

#include <support/cleanse.h>

#include <cstddef>
#include <memory>
#include <string>

template <typename T>
struct secure_allocator {
    using value_type = T;
    secure_allocator() noexcept = default;
    template <typename U>
    secure_allocator(const secure_allocator<U>&) noexcept {}
    T* allocate(std::size_t n) { return std::allocator<T>{}.allocate(n); }
    void deallocate(T* p, std::size_t n) noexcept
    {
        if (p) memory_cleanse(p, sizeof(T) * n);
        std::allocator<T>{}.deallocate(p, n);
    }
    template <typename U>
    bool operator==(const secure_allocator<U>&) const noexcept { return true; }
};

using SecureString = std::basic_string<char, std::char_traits<char>, secure_allocator<char>>;
