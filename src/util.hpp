#pragma once
#include "types.hpp"

template<typename T>
inline bool bit(T v, int n) {
    return (v >> n) & 1;
}

template<typename T>
inline T set_bit(T v, int n, bool on) {
    if (on) return v | (T(1) << n);
    return v & ~(T(1) << n);
}

inline u32 ror32(u32 v, int amount) {
    amount &= 31;
    if (amount == 0) return v;
    return (v >> amount) | (v << (32 - amount));
}

inline u32 align2(u32 v) { return v & ~1u; }
inline u32 align4(u32 v) { return v & ~3u; }

inline s32 sign_extend24(u32 v) {
    return (v & 0x00800000) ? static_cast<s32>(v | 0xFF000000) : static_cast<s32>(v);
}

inline s32 sign_extend11(u32 v) {
    return (v & 0x400) ? static_cast<s32>(v | 0xFFFFF800) : static_cast<s32>(v);
}

inline s32 sign_extend8(u32 v) {
    return (v & 0x80) ? static_cast<s32>(v | 0xFFFFFF00) : static_cast<s32>(v);
}

inline bool add_carry(u32 a, u32 b) {
    return static_cast<u64>(a) + static_cast<u64>(b) > 0xFFFFFFFFull;
}

inline bool add_overflow(u32 a, u32 b, u32 r) {
    return ((~(a ^ b) & (a ^ r)) >> 31) & 1;
}

inline bool sub_overflow(u32 a, u32 b, u32 r) {
    return (((a ^ b) & (a ^ r)) >> 31) & 1;
}
