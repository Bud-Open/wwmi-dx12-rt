#pragma once
// ===========================================================================
//  tex_struct_recipe_cache.hpp -- Lock-free per-thread ordered material
//  recipe cache + two-phase PublishSeq for StructFallbackEngine::Apply.
//
//  Platform-independent C++20 header included directly by both production
//  code (`tex_struct_fallback.hpp`) and unit tests.
// ===========================================================================
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

namespace wwmi_struct_tex {

// Two-phase publication sequence counter:
// - High 16 bits: number of in-flight publishers (nonzero => publication in progress).
// - Low 48 bits: monotonic completed publication version.
//
// If a reader observes `s0 == s1 && is_stable(s0)`, no publisher was active
// before or after AND no publication completed across the interval — closing
// the post-write-only epoch race against lock-free readers (e.g. TexLod/TexRules).
class PublishSeq {
    static constexpr uint64_t kInFlightStep = 1ull << 48;
    static constexpr uint64_t kInFlightMask = 0xFFFF000000000000ull;
    std::atomic<uint64_t>     raw_{ 0 };

public:
    class Guard {
        PublishSeq* seq_ = nullptr;

    public:
        explicit Guard(PublishSeq& seq) noexcept : seq_(&seq)
        {
            seq_->begin_publish();
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&& other) noexcept : seq_(other.seq_) { other.seq_ = nullptr; }
        ~Guard() noexcept
        {
            if (seq_) seq_->end_publish();
        }
    };

    void begin_publish() noexcept
    {
        raw_.fetch_add(kInFlightStep, std::memory_order_seq_cst);
    }
    void end_publish() noexcept
    {
        // Atomically decrement in_flight by 1 and increment version by 1.
        raw_.fetch_add(1ull - kInFlightStep, std::memory_order_seq_cst);
    }
    void bump() noexcept
    {
        Guard g(*this);
    }
    uint64_t snapshot() const noexcept
    {
        return raw_.load(std::memory_order_acquire);
    }
    static constexpr bool is_stable(uint64_t s) noexcept
    {
        return (s & kInFlightMask) == 0;
    }
};

struct RecipeSlotSnap {
    uint8_t  reg = 0;
    uint8_t  param = 0xFF;
    uint8_t  pre_ovr = 0;
    uint8_t  pad = 0;
    uint32_t offset = 0;
    uint32_t vfmt = 0;
    uint32_t hash = 0;
    uint64_t res_h = 0;
    uint64_t res_gen = 0;

    bool operator==(const RecipeSlotSnap&) const noexcept = default;
};

struct RecipeKey {
    unsigned       load = 0;
    int            dm = -1;
    int            comp = -1;
    uint64_t       ps_hash = 0;
    uint64_t       ps_regs_lo = 0;
    uint64_t       layh = 0;
    uint8_t        ps_decl_known = 0;
    uint8_t        lod_flags = 0; // bit0: alias_off, bit1: resemblance, bit2: cache
    uint8_t        active_count = 0;
    RecipeSlotSnap slots[8]{};

    bool operator==(const RecipeKey& o) const noexcept
    {
        if (load != o.load || dm != o.dm || comp != o.comp || ps_hash != o.ps_hash ||
            ps_regs_lo != o.ps_regs_lo || layh != o.layh || ps_decl_known != o.ps_decl_known ||
            lod_flags != o.lod_flags || active_count != o.active_count)
            return false;
        for (uint8_t i = 0; i < active_count; ++i)
            if (!(slots[i] == o.slots[i])) return false;
        return true;
    }

    uint64_t hash_value() const noexcept
    {
        auto mix = [](uint64_t h, uint64_t v) noexcept -> uint64_t {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            return h;
        };
        uint64_t h = 0xcbf29ce484222325ull;
        h = mix(h, (static_cast<uint64_t>(load) << 32) | static_cast<uint32_t>(dm));
        h = mix(h, (static_cast<uint64_t>(static_cast<uint32_t>(comp)) << 32) |
                       (static_cast<uint64_t>(active_count) << 16) |
                       (static_cast<uint64_t>(ps_decl_known) << 8) | lod_flags);
        h = mix(h, ps_hash);
        h = mix(h, ps_regs_lo);
        h = mix(h, layh);
        for (uint8_t i = 0; i < active_count; ++i) {
            const auto& s = slots[i];
            h = mix(h, s.res_h);
            h = mix(h, s.res_gen);
            h = mix(h, (static_cast<uint64_t>(s.hash) << 32) | s.vfmt);
            h = mix(h, (static_cast<uint64_t>(s.offset) << 32) | (static_cast<uint64_t>(s.reg) << 24) |
                           (static_cast<uint64_t>(s.param) << 16) | (static_cast<uint64_t>(s.pre_ovr) << 8));
        }
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33;
        return h;
    }
};

struct RecipeBind {
    uint8_t      reg = 0;
    uint8_t      param = 0xFF;
    uint32_t     offset = 0;
    uint32_t     vfmt = 0;
    std::wstring fq;
};

struct RecipeEntry {
    RecipeKey   key{};
    uint64_t    state_seq = 0;
    uint64_t    meta_seq = 0;
    uint8_t     bind_count = 0;
    bool        valid = false;
    RecipeBind  binds[8]{};
};

// Lock-free per-thread 64-set x 2-way associative cache (128 ordered material recipes).
class ThreadRecipeCache {
    static constexpr size_t kSets = 64;
    struct Set {
        RecipeEntry ways[2]{};
        uint8_t     next_victim = 0;
    };
    std::array<Set, kSets> sets_{};

public:
    const RecipeEntry* lookup(const RecipeKey& key, uint64_t state_seq, uint64_t meta_seq) const noexcept
    {
        if (!PublishSeq::is_stable(state_seq) || !PublishSeq::is_stable(meta_seq)) return nullptr;
        const size_t si = static_cast<size_t>(key.hash_value()) & (kSets - 1);
        const auto&  set = sets_[si];
        for (const auto& e : set.ways) {
            if (e.valid && e.state_seq == state_seq && e.meta_seq == meta_seq && e.key == key)
                return &e;
        }
        return nullptr;
    }

    void invalidate(const RecipeKey& key) noexcept
    {
        const size_t si = static_cast<size_t>(key.hash_value()) & (kSets - 1);
        auto&        set = sets_[si];
        for (auto& e : set.ways) {
            if (e.valid && e.key == key) e.valid = false;
        }
    }

    void store(const RecipeKey& key, uint64_t state_seq, uint64_t meta_seq, const RecipeBind* binds, uint8_t bind_count)
    {
        if (!PublishSeq::is_stable(state_seq) || !PublishSeq::is_stable(meta_seq) || bind_count > 8) return;
        const size_t si = static_cast<size_t>(key.hash_value()) & (kSets - 1);
        auto&        set = sets_[si];
        size_t       target = 2;
        for (size_t w = 0; w < 2; ++w) {
            if (set.ways[w].valid && set.ways[w].key == key) {
                target = w;
                break;
            }
        }
        if (target == 2) {
            for (size_t w = 0; w < 2; ++w) {
                if (!set.ways[w].valid) {
                    target = w;
                    break;
                }
            }
        }
        if (target == 2) {
            target = set.next_victim & 1u;
        }
        set.next_victim = static_cast<uint8_t>(target ^ 1u);
        auto& dst = set.ways[target];
        dst.key = key;
        dst.state_seq = state_seq;
        dst.meta_seq = meta_seq;
        dst.bind_count = bind_count;
        for (uint8_t i = 0; i < bind_count; ++i) dst.binds[i] = binds[i];
        for (uint8_t i = bind_count; i < 8; ++i) dst.binds[i] = {};
        dst.valid = true;
    }

    void clear() noexcept
    {
        for (auto& set : sets_) {
            set.ways[0].valid = false;
            set.ways[1].valid = false;
            set.next_victim = 0;
        }
    }
};

// Generic epoch-validated per-thread 16-entry cache for pipeline layout PS register maps.
template <typename RegBindT>
class ThreadLayoutPsCache {
    static constexpr size_t kSlots = 16;
    struct Entry {
        uint64_t layh = 0;
        uint64_t epoch = 0;
        bool     built = false;
        bool     valid = false;
        RegBindT t_ps[8]{};
    };
    std::array<Entry, kSlots> entries_{};

    static size_t index_of(uint64_t layh) noexcept
    {
        uint64_t h = layh;
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdull;
        return static_cast<size_t>(h >> 33) & (kSlots - 1);
    }

public:
    bool find(uint64_t layh, uint64_t epoch, bool& out_built, RegBindT (&out_t_ps)[8]) const noexcept
    {
        const auto& e = entries_[index_of(layh)];
        if (!e.valid || e.epoch != epoch || e.layh != layh) return false;
        out_built = e.built;
        if (e.built) std::memcpy(out_t_ps, e.t_ps, sizeof(out_t_ps));
        return true;
    }

    void put(uint64_t layh, uint64_t epoch, bool built, const RegBindT (&t_ps)[8]) noexcept
    {
        auto& e = entries_[index_of(layh)];
        e.layh = layh;
        e.epoch = epoch;
        e.built = built;
        e.valid = true;
        if (built) std::memcpy(e.t_ps, t_ps, sizeof(e.t_ps));
    }

    void clear() noexcept
    {
        for (auto& e : entries_) e.valid = false;
    }
};

struct ObsLodRecord {
    uint32_t lod = 0;
    uint32_t vfmt = 0;
    uint32_t max_dim = 0;
    bool     missing_sig = false;
};

// Upsert an observed visual key on (dm, comp). Returns true when new evidence was recorded
// (new key, higher mip resolution, or a key whose signature was previously missing on the worker
// has now become available again). Genuinely evaluated ambiguous/flat rows keep missing_sig=false
// and stay settled.
inline bool UpsertObservedLod(std::vector<ObsLodRecord>& vec, uint32_t lod, uint32_t vfmt, uint32_t dim)
{
    for (auto& e : vec) {
        if (e.lod != lod) continue;
        if (dim <= e.max_dim && !e.missing_sig) return false;
        if (dim > e.max_dim) e.max_dim = dim;
        e.missing_sig = false;
        return true;
    }
    vec.push_back(ObsLodRecord{ lod, vfmt, dim, false });
    return true;
}

// Reconcile per-row signature availability observed by the worker into the live observation list.
// Returns true if any entry's missing_sig bit changed.
inline bool ApplyWorkerSigAvailability(
    std::vector<ObsLodRecord>& live_vec,
    const std::vector<ObsLodRecord>& worker_obs,
    const std::vector<uint8_t>& worker_missing_sig,
    bool evidence_unchanged)
{
    bool changed = false;
    const size_t n = (std::min)(worker_obs.size(), worker_missing_sig.size());
    for (size_t i = 0; i < n; ++i) {
        const uint32_t lod = worker_obs[i].lod;
        const bool     was_missing = (worker_missing_sig[i] != 0u);
        for (auto& e : live_vec) {
            if (e.lod != lod) continue;
            if (was_missing) {
                if (evidence_unchanged && !e.missing_sig) {
                    e.missing_sig = true;
                    changed = true;
                }
            } else if (e.missing_sig) {
                e.missing_sig = false;
                changed = true;
            }
            break;
        }
    }
    return changed;
}

} // namespace wwmi_struct_tex

wwmi_struct_tex::PublishSeq& TexStructMetaSeq() noexcept;
