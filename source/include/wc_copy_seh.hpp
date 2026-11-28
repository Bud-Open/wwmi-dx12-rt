#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <smmintrin.h>
#include <windows.h>

namespace wwmi {

// Non-inlined leaf so clang-cl (/O2 /EHsc) always emits a real SEH table entry
// (__C_specific_handler) around the call site in safe_copy_from_wc().
__declspec(noinline) inline void copy_from_wc_leaf(void *dst, const void *src, std::size_t n) noexcept {
    auto *d = static_cast<unsigned char *>(dst);
    const auto *s = static_cast<const unsigned char *>(src);
    if (n < 32) {
        std::memcpy(d, s, n);
        return;
    }
    const std::uintptr_t mis = reinterpret_cast<std::uintptr_t>(s) & 15u;
    if (mis) {
        const std::size_t head = 16u - mis;
        std::memcpy(d, s, head);
        d += head;
        s += head;
        n -= head;
    }
    // Intel SDM Vol. 1 §12.10.3: MOVNTDQA is weakly ordered on WC memory;
    // bracket streaming loads with MFENCE so they cannot pass producer handoff
    // loads or subsequent memory operations.
    _mm_mfence();
    while (n >= 64) {
        const __m128i v0 = _mm_stream_load_si128(reinterpret_cast<const __m128i *>(s));
        const __m128i v1 = _mm_stream_load_si128(reinterpret_cast<const __m128i *>(s + 16));
        const __m128i v2 = _mm_stream_load_si128(reinterpret_cast<const __m128i *>(s + 32));
        const __m128i v3 = _mm_stream_load_si128(reinterpret_cast<const __m128i *>(s + 48));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(d), v0);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(d + 16), v1);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(d + 32), v2);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(d + 48), v3);
        d += 64;
        s += 64;
        n -= 64;
    }
    while (n >= 16) {
        const __m128i v = _mm_stream_load_si128(reinterpret_cast<const __m128i *>(s));
        _mm_storeu_si128(reinterpret_cast<__m128i *>(d), v);
        d += 16;
        s += 16;
        n -= 16;
    }
    _mm_mfence();
    if (n) {
        std::memcpy(d, s, n);
    }
}

inline DWORD filter_mapped_read_fault(DWORD code) noexcept {
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
        ? EXCEPTION_EXECUTE_HANDLER
        : EXCEPTION_CONTINUE_SEARCH;
}

// Keep this SEH wrapper non-inlined and free of C++ RAII objects so both
// clang-cl and MSVC cl (/O2 /EHsc) emit a dedicated __C_specific_handler frame
// isolated from callers that own C++ destructors.
__declspec(noinline) inline bool safe_copy_from_wc(void *dst, const void *src, std::size_t n) noexcept {
    if (!n) return true;
    if (!dst || !src) return false;
    __try {
        copy_from_wc_leaf(dst, src, n);
        return true;
    } __except (filter_mapped_read_fault(GetExceptionCode())) {
        return false;
    }
}

} // namespace wwmi
