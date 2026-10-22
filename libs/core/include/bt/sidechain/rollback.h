// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// lib/state/rollback.rs of the LayerTwo-family chains (identical in the plain-bitnames fork
// v0.18.2 and the plain-bitassets fork v0.17.1): a NonEmpty history of values stamped with a
// height (RollBack<HeightStamped<T>>) or with a txid and a height (RollBack<TxidStamped<T>>).
#pragma once

#include <bt/bytes.h>

#include <optional>
#include <utility>
#include <vector>

namespace bt::sidechain {

template <class T>
struct HeightStamped {
    T value;
    uint32_t height;
};

/** RollBack<HeightStamped<T>>: NonEmpty, push refuses a lower height, pop keeps the head. */
template <class T>
struct RollBack {
    std::vector<HeightStamped<T>> v; // never empty
    static RollBack make(T x, uint32_t h) { return RollBack{{{x, h}}}; }
    const HeightStamped<T>& latest() const { return v.back(); }
    const HeightStamped<T>& earliest() const { return v.front(); }
    bool push(T x, uint32_t h)
    {
        if (v.back().height > h) return false;
        v.push_back({x, h});
        return true;
    }
    /** (rest, popped): rest is nullopt when only the head was there (then popped = head). */
    std::pair<std::optional<RollBack>, HeightStamped<T>> pop() const
    {
        if (v.size() > 1) {
            RollBack r = *this;
            auto last = r.v.back();
            r.v.pop_back();
            return {r, last};
        }
        return {std::nullopt, v.front()};
    }
};

template <class T>
struct TxidStamped {
    T data;
    Hash32 txid;
    uint32_t height;
};

/** RollBack<TxidStamped<T>>: NonEmpty, push without checks, pop keeps the head. */
template <class T>
struct TxHistory {
    std::vector<TxidStamped<T>> v; // never empty
    static TxHistory make(T x, const Hash32& txid, uint32_t h) { return TxHistory{{TxidStamped<T>{std::move(x), txid, h}}}; }
    const TxidStamped<T>& latest() const { return v.back(); }
    void push(T x, const Hash32& txid, uint32_t h) { v.push_back(TxidStamped<T>{std::move(x), txid, h}); }
    /** NonEmpty::pop: the last element, or nullopt when only the head remains. */
    std::optional<TxidStamped<T>> pop()
    {
        if (v.size() < 2) return std::nullopt;
        auto r = v.back();
        v.pop_back();
        return r;
    }
    /** at_block_height: the last value stamped at or below height. */
    const TxidStamped<T>* at_block_height(uint32_t h) const
    {
        for (auto it = v.rbegin(); it != v.rend(); ++it)
            if (it->height <= h) return &*it;
        return nullptr;
    }
};

} // namespace bt::sidechain
