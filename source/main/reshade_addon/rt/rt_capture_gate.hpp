#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
namespace rt_capture {
constexpr uint32_t kLimit = 16;
constexpr uint32_t kNoReceipt = 0xffffffffu;
struct Key {
    int component;
    uint32_t count, first;
    int32_t base;
    bool operator==(const Key &b) const noexcept {
        return component == b.component && count == b.count && first == b.first && base == b.base;
    }
};
struct Gate {
    struct Entry {std::wstring path;Key draw{};};
    std::array<Entry,kLimit> keys{};
    uint32_t count = 0;
    bool target_seen = false, overflow = false;
    uint32_t accept(std::wstring_view path, Key k) {
        if (path.empty()) return kNoReceipt;
        target_seen = true;
        if (k.component < 0 || !k.count) return kNoReceipt;
        for (uint32_t i = 0; i < count; ++i) if (keys[i].path == path && keys[i].draw == k) return kNoReceipt;
        if (count == kLimit) { overflow = true; return kNoReceipt; }
        keys[count] = {std::wstring(path),k}; return count++;
    }
};
} // namespace rt_capture
