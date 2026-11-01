#pragma once

// Inlining control for the search path. bbtree measured that the node
// search adapters must be inlined into every descent and that tie searches
// must stay out of it (D36, D41, D49): left to the inliner, each level
// returned its state through memory, or a rarely taken tie search was
// inlined into every level. These say so to GCC and Clang; other compilers
// decide for themselves.

#if defined(__GNUC__) || defined(__clang__)
#define STX_BTREE_ALWAYS_INLINE __attribute__((always_inline)) inline
#define STX_BTREE_NEVER_INLINE __attribute__((noinline))
#else
#define STX_BTREE_ALWAYS_INLINE inline
#define STX_BTREE_NEVER_INLINE
#endif
