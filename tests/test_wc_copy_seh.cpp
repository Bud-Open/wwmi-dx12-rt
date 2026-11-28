#include "wc_copy_seh.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int fail(const char *msg) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

int main() {
    // 1. Edge cases: zero length and null pointers.
    unsigned char dummy[8]{};
    if (!wwmi::safe_copy_from_wc(nullptr, nullptr, 0))
        return fail("zero-length copy should succeed");
    if (wwmi::safe_copy_from_wc(dummy, nullptr, 4))
        return fail("null src with nonzero length should fail");
    if (wwmi::safe_copy_from_wc(nullptr, dummy, 4))
        return fail("null dst with nonzero length should fail");

    // 2. Exhaustive alignment and size matrix covering scalar, head/tail, 16B and 64B SSE4.1 loops.
    const std::size_t sizes[] = {1, 3, 7, 15, 16, 17, 28, 31, 32, 33, 47, 48, 63, 64, 65, 95, 127, 128, 131, 511, 4096, 24576};
    std::vector<unsigned char> src_buf(24576 + 64);
    std::vector<unsigned char> dst_buf(24576 + 64);
    for (std::size_t i = 0; i < src_buf.size(); ++i)
        src_buf[i] = static_cast<unsigned char>((i * 131u + 17u) & 0xFFu);

    for (std::size_t s : sizes) {
        for (std::size_t s_off = 0; s_off < 16; s_off += (s > 256 ? 5 : 1)) {
            for (std::size_t d_off = 0; d_off < 16; d_off += (s > 256 ? 7 : 3)) {
                std::memset(dst_buf.data(), 0xA5, s + 32);
                const unsigned char *sp = src_buf.data() + s_off;
                unsigned char *dp = dst_buf.data() + 16 + d_off;
                if (!wwmi::safe_copy_from_wc(dp, sp, s))
                    return fail("valid copy returned false");
                if (std::memcmp(dp, sp, s) != 0)
                    return fail("copied bytes mismatch");
                // Guard bytes around dst must remain untouched.
                if (dp[-1] != 0xA5 || dp[s] != 0xA5)
                    return fail("destination buffer overrun/underrun");
            }
        }
    }

    // 3. Real OS page-fault SEH tests (PAGE_NOACCESS and straddling committed -> PAGE_NOACCESS).
    auto *pages = static_cast<unsigned char *>(
        VirtualAlloc(nullptr, 2 * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!pages)
        return fail("VirtualAlloc failed");
    for (std::size_t i = 0; i < 4096; ++i)
        pages[i] = static_cast<unsigned char>(i & 0xFFu);
    DWORD old_prot = 0;
    if (!VirtualProtect(pages + 4096, 4096, PAGE_NOACCESS, &old_prot)) {
        VirtualFree(pages, 0, MEM_RELEASE);
        return fail("VirtualProtect PAGE_NOACCESS failed");
    }

    unsigned char out[256]{};
    // 3a. Fault on scalar (<32B) path directly on PAGE_NOACCESS.
    if (wwmi::safe_copy_from_wc(out, pages + 4096, 28)) {
        VirtualFree(pages, 0, MEM_RELEASE);
        return fail("28-byte read on PAGE_NOACCESS unexpectedly succeeded");
    }
    // 3b. Fault on SSE4.1 (>=64B) path directly on PAGE_NOACCESS.
    if (wwmi::safe_copy_from_wc(out, pages + 4096, 128)) {
        VirtualFree(pages, 0, MEM_RELEASE);
        return fail("128-byte read on PAGE_NOACCESS unexpectedly succeeded");
    }
    // 3c. Fault mid-copy straddling committed page into PAGE_NOACCESS (64B loop).
    if (wwmi::safe_copy_from_wc(out, pages + 4096 - 32, 96)) {
        VirtualFree(pages, 0, MEM_RELEASE);
        return fail("straddling 96-byte read into PAGE_NOACCESS unexpectedly succeeded");
    }
    // 3d. Valid read right up to the exact last byte of the committed page must still succeed.
    if (!wwmi::safe_copy_from_wc(out, pages + 4096 - 96, 96) ||
        std::memcmp(out, pages + 4096 - 96, 96) != 0) {
        VirtualFree(pages, 0, MEM_RELEASE);
        return fail("valid 96-byte read at exact page end failed");
    }

    VirtualFree(pages, 0, MEM_RELEASE);
    std::printf("OK: wc_copy_seh tests passed\n");
    return 0;
}
