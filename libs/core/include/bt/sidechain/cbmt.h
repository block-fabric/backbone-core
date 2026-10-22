// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// merkle-cbt 0.3.2 CBMT::build_merkle_root, the complete binary merkle tree every
// LayerTwo-family chain uses for its outputs, inputs and transactions:
//   * no leaves: T::default();
//   * pair leaves from the end (rchunks_exact(2)), the odd first leaf goes to the front;
//   * then repeatedly pop two from the front (right first, then left: the queue holds the
//     right sibling ahead of the left one) and push their parent to the back.
// `merge(left, right)` returns nullopt where theirs carries an error value up the tree; the
// whole root is then nullopt.
#pragma once

#include <deque>
#include <optional>
#include <vector>

namespace bt::sidechain {

template <class T, class Merge>
std::optional<T> cbmt_root(const std::vector<T>& leaves, Merge merge)
{
    if (leaves.empty()) return T{};
    std::deque<T> q;
    size_t i = leaves.size();
    while (i >= 2) {
        auto m = merge(leaves[i - 2], leaves[i - 1]);
        if (!m) return std::nullopt;
        q.push_back(std::move(*m));
        i -= 2;
    }
    if (i == 1) q.push_front(leaves[0]);
    while (q.size() > 1) {
        T right = std::move(q.front());
        q.pop_front();
        T left = std::move(q.front());
        q.pop_front();
        auto m = merge(left, right);
        if (!m) return std::nullopt;
        q.push_back(std::move(*m));
    }
    return q.front();
}

} // namespace bt::sidechain
