#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace wwmi_tex_live_sig {

constexpr size_t kSoftMaxLiveSigs = 2048;
constexpr size_t kHardMaxLiveSigs = 4096;
constexpr size_t kEvictScanBatch = 64;

// O(1) resident carrier refcount table maintained under the texture hash map's existing writer lock.
// Handles multiple resources carrying the same content hash as well as hash overwrites/restores.
class ResidentHashRefCounts {
public:
    void OnSet(uint32_t old_hash, uint32_t new_hash)
    {
        if (old_hash == new_hash) return;
        if (old_hash) Dec(old_hash);
        if (new_hash) ++counts_[new_hash];
    }

    void OnErase(uint32_t old_hash)
    {
        if (old_hash) Dec(old_hash);
    }

    bool IsResident(uint32_t hash) const
    {
        return hash != 0 && counts_.find(hash) != counts_.end();
    }

    uint32_t Count(uint32_t hash) const
    {
        auto it = counts_.find(hash);
        return it != counts_.end() ? it->second : 0u;
    }

    size_t UniqueHashes() const noexcept { return counts_.size(); }

private:
    void Dec(uint32_t hash)
    {
        auto it = counts_.find(hash);
        if (it == counts_.end()) return;
        if (it->second <= 1u)
            counts_.erase(it);
        else
            --it->second;
    }

    std::unordered_map<uint32_t, uint32_t> counts_;
};

// Step 1 (caller holds sig-map lock): peek up to kEvictScanBatch oldest hashes without mutating state.
inline std::vector<uint32_t> PeekEvictCandidates(
    const std::deque<uint32_t>& order, size_t map_size,
    size_t soft_max = kSoftMaxLiveSigs, size_t batch = kEvictScanBatch)
{
    std::vector<uint32_t> out;
    if (map_size < soft_max || order.empty()) return out;
    const size_t n = (std::min)(order.size(), batch);
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(order[i]);
    return out;
}

// Step 3 (caller re-acquired sig-map lock after querying residency unlocked):
// Evicts non-resident entries from the front of `order` first until `map.size() < soft_max`,
// rotates inspected still-resident entries to the back so subsequent inserts inspect newer entries,
// and enforces `hard_max` as a strict upper bound even when all entries remain resident.
template <typename MapT>
inline size_t ApplyResidentFirstEviction(
    MapT& sigs, std::deque<uint32_t>& order,
    const std::vector<uint32_t>& candidates, const std::vector<uint8_t>& resident_flags,
    size_t soft_max = kSoftMaxLiveSigs, size_t hard_max = kHardMaxLiveSigs)
{
    size_t evicted = 0;
    const size_t n = (std::min)(candidates.size(), resident_flags.size());
    auto lookup_status = [&](uint32_t h) -> int {
        for (size_t i = 0; i < n; ++i)
            if (candidates[i] == h) return resident_flags[i] ? 1 : 0;
        return -1;
    };

    size_t inspected = 0;
    while (sigs.size() >= soft_max && !order.empty() && inspected < n) {
        const uint32_t h = order.front();
        const int      st = lookup_status(h);
        if (st < 0) break; // front moved concurrently; stop scanning unknown entries
        order.pop_front();
        ++inspected;
        auto it = sigs.find(h);
        if (it == sigs.end()) continue;
        if (st == 0) {
            sigs.erase(it);
            ++evicted;
        } else {
            order.push_back(h);
        }
    }

    while (sigs.size() >= hard_max && !order.empty()) {
        const uint32_t h = order.front();
        order.pop_front();
        if (sigs.erase(h)) ++evicted;
    }
    return evicted;
}

} // namespace wwmi_tex_live_sig
