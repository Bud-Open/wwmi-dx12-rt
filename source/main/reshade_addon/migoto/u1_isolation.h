#pragma once
// Narrow provenance for the single shared port UAV slot used by ShapeKey.
// The tiny provenance lock protects only these records, not the engine or GPU calls.
// Active instance and restore nesting are scoped to the executing thread.
#include <cstdint>
#include <mutex>

namespace MigotoPort::U1Isolation {

struct Identity { int owner = -1; const void* inst = nullptr; };
inline bool Same(Identity a, Identity b) { return a.owner == b.owner && a.inst == b.inst; }
struct Context { Identity who{}; bool active = false; };
inline thread_local Context current{};
inline Context CurrentScope() { return current; }

class Scope {
    Context prior;
public:
    explicit Scope(Context next) : prior(current) { current = next; }
    Scope(int owner, const void* inst, bool enabled = true) : prior(current)
    { current = {{owner, inst}, enabled && owner >= 0 && inst != nullptr}; }
    ~Scope() { current = prior; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

enum class Origin : uint8_t { Unknown, Game, Explicit, Dead };
struct Mark {
    Origin origin = Origin::Unknown;
    Identity who{};
    const void* view = nullptr;
    const void* wrapper = nullptr;
    const void* resource = nullptr;
    uint64_t epoch = 0;
    bool descriptor_ready = false;
};
inline std::mutex tracker_lock;
inline Mark binding{};
inline Mark port_content{};
inline uint64_t content_epoch = 0;
inline uint64_t setter_sequence = 0;
inline thread_local unsigned port_depth = 0;

// Only the active custom-shader OM saves are registered; a destroy notification can
// invalidate the saved u1 before the automatic OM restore. The node is stack-owned.
struct Saved;
inline Saved* active_saves = nullptr;
struct Saved {
    Mark mark{};
    bool invalidated = false;
    Saved* next = nullptr;
    bool registered = false;
    Saved() {
        std::lock_guard<std::mutex> lock(tracker_lock);
        mark = binding;
        if (mark.view && mark.origin != Origin::Unknown) {
            registered = true;
            next = active_saves;
            active_saves = this;
        }
    }
    ~Saved() {
        if (!registered) return;
        std::lock_guard<std::mutex> lock(tracker_lock);
        Saved** link = &active_saves;
        while (*link && *link != this) link = &(*link)->next;
        if (*link == this) *link = next;
    }
    Saved(const Saved&) = delete;
    Saved& operator=(const Saved&) = delete;
};
inline thread_local const Saved* om_restore = nullptr;

struct PortPublishGuard {
    PortPublishGuard() { ++port_depth; }
    ~PortPublishGuard() { --port_depth; }
    PortPublishGuard(const PortPublishGuard&) = delete;
};
inline Saved Capture() { return Saved{}; }
struct OmRestoreScope {
    const Saved* prior;
    explicit OmRestoreScope(const Saved& saved) : prior(om_restore) { om_restore = &saved; }
    ~OmRestoreScope() { om_restore = prior; }
    OmRestoreScope(const OmRestoreScope&) = delete;
};
inline bool CurrentEpoch(const Mark& m)
{ return !m.wrapper || m.wrapper != port_content.wrapper || m.epoch == port_content.epoch; }

// The slot's stable wrapper can acquire different backing even if no setter runs.
inline void OnPortMutation(const void* wrapper, const void* resource)
{
    std::lock_guard<std::mutex> lock(tracker_lock);
    ++content_epoch;
    port_content = {};
    port_content.origin = current.active ? Origin::Game : Origin::Unknown;
    port_content.who = current.who;
    port_content.wrapper = wrapper;
    port_content.resource = resource;
    port_content.epoch = content_epoch;
    if (binding.view && binding.wrapper == wrapper) {
        const void* view = binding.view;
        binding = port_content;
        binding.view = view;
    }
}

inline void OnDescriptorWritten(const void* wrapper, const void* resource)
{
    std::lock_guard<std::mutex> lock(tracker_lock);
    if (port_content.wrapper != wrapper || port_content.resource != resource) return;
    port_content.descriptor_ready = true;
    if (binding.view && binding.wrapper == wrapper) binding.descriptor_ready = true;
}
inline bool Published(const void* resource)
{
    std::lock_guard<std::mutex> lock(tracker_lock);
    return current.active && binding.view && binding.origin == Origin::Game &&
           Same(binding.who, current.who) && binding.resource == resource && binding.descriptor_ready;
}
inline void FailedPublish()
{
    std::lock_guard<std::mutex> lock(tracker_lock);
    if (!current.active || !Same(port_content.who, current.who)) return;
    port_content.origin = Origin::Dead;
    if (binding.view && binding.wrapper == port_content.wrapper) binding.origin = Origin::Dead;
}

// Called on every real CS/OM slot-1 setter, including same-view assignments.
inline void OnSet(const void* before, const void* after, const void* wrapper, const void* resource)
{
    std::lock_guard<std::mutex> lock(tracker_lock);
    ++setter_sequence;
    if (om_restore) {
        if (before == after && binding.view == after && binding.origin == Origin::Explicit && CurrentEpoch(binding)) return;
        const Mark& saved = om_restore->mark;
        if (saved.view == after && saved.wrapper == wrapper && CurrentEpoch(saved)) {
            binding = saved;
            if (om_restore->invalidated) binding.origin = Origin::Dead;
            return;
        }
        if (wrapper && wrapper == port_content.wrapper) {
            binding = port_content;
            binding.view = after;
            return;
        }
        binding = {};
        binding.view = after;
        binding.wrapper = wrapper;
        binding.resource = resource;
        return;
    }
    binding = {};
    binding.view = after;
    binding.wrapper = wrapper;
    binding.resource = resource;
    if (port_depth && wrapper && wrapper == port_content.wrapper) {
        binding = port_content;
        binding.view = after;
    } else if (current.active) {
        binding.origin = Origin::Explicit;
        binding.who = current.who;
        if (wrapper && wrapper == port_content.wrapper) {
            binding.epoch = port_content.epoch;
            binding.resource = port_content.resource;
        }
    }
}
inline void Forget(const void* dead)
{
    if (!dead) return;
    std::lock_guard<std::mutex> lock(tracker_lock);
    if (binding.who.inst == dead || binding.resource == dead) binding.origin = Origin::Dead;
    if (port_content.who.inst == dead || port_content.resource == dead) port_content.origin = Origin::Dead;
    for (Saved* saved = active_saves; saved; saved = saved->next)
        if (saved->mark.who.inst == dead || saved->mark.resource == dead) saved->invalidated = true;
}
inline bool ShouldMask(const void* view)
{
    if (!current.active || !view) return false;
    std::lock_guard<std::mutex> lock(tracker_lock);
    if (binding.view != view) return false;
    Mark effective = binding;
    if (!CurrentEpoch(effective)) {
        if (effective.wrapper != port_content.wrapper) return false;
        effective = port_content;
    }
    if (effective.origin == Origin::Dead) return true;
    return (effective.origin == Origin::Game || effective.origin == Origin::Explicit) &&
           !Same(effective.who, current.who);
}
inline uint64_t SetterSequence() { std::lock_guard<std::mutex> lock(tracker_lock); return setter_sequence; }

} // namespace MigotoPort::U1Isolation
