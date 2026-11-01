#pragma once

// The one place that issues a software prefetch: a read hint for the line a
// batch lane reads next (batch.hpp, optimal_map.hpp). A no-op where the
// compiler has no hint. Prefetch hints are issued nowhere else in the
// library (bbtree D46).

#include "stx_btree/detail/inlining.hpp"

namespace stx_btree::detail {

class Prefetcher final {
public:
    STX_BTREE_ALWAYS_INLINE static void hint(const void* line) noexcept {
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(line, 0, 3);
#else
        (void)line;
#endif
    }
};

} // namespace stx_btree::detail
