#pragma once
#include "hsin_pose.hpp"
#include "rt_pack_identity_v2.h"
#include <optional>
#include <d3d12.h>
#include <unordered_map>
namespace hsin_rt {
struct NativeComponentIdentity {
    SkinSample sample;
    GeometryInput vertices; // full original VertexCount span, independent per ring
    ResourceKey indices;
    std::uint64_t vertex_address=0,index_address=0,index_offset=0;
    unsigned index_count=0,index_stride=4;
};
struct NativeBlasIdentity {
    Recording recording;
    ResourceKey allocation;
    std::uint64_t native_blas=0;
    std::vector<NativeComponentIdentity> components;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
};
struct DrawIdentityQuery {
    Recording recording;
    ResourceKey vertices,indices;
    std::uint64_t vertex_address=0,vertex_bytes=0,index_address=0,index_bytes=0;
    unsigned index_count=0;
};
struct NativeIdentityFence {
    std::uint64_t queue=0,fence=0,value=0;
    bool operator==(const NativeIdentityFence &) const=default;
};
// Runtime's device mutex owns this state, exactly as it owns InputCollector.
// No command queues, GPU resources, callbacks or worker threads are created here.
class NativeDrawIdentityCache {
    struct Entry {
        std::shared_ptr<const NativeBlasIdentity> identity;
        NativeIdentityFence producer;
        unsigned submissions=0;
        bool valid=true;
        std::vector<bool> components_live;
        std::uint64_t snapshot=0;
    };
    std::vector<std::shared_ptr<Entry>> entries_;
    std::unordered_map<std::uint64_t,std::uint64_t> completed_;
    std::uint64_t next_snapshot_=0;
    std::uint64_t validity_revision_=0;
    void compact();
public:
    struct Frozen {std::vector<std::shared_ptr<Entry>> entries;};
    struct Match {
        std::shared_ptr<const NativeBlasIdentity> identity;
        unsigned component=0,ordering=0;
        NativeIdentityFence producer;
        std::uint64_t observed_completion=0;
        unsigned submissions=0;
        std::uint64_t snapshot=0;
    };
    struct LookupReceipt {
        std::optional<Match> first_exact;
        unsigned exact_matches=0;
        bool ambiguous=false;
    };
    void record(std::shared_ptr<const NativeBlasIdentity>);
    void record_with_state(std::shared_ptr<const NativeBlasIdentity>,unsigned submissions,NativeIdentityFence producer);
    void reset(Recording); // unsubmitted records are discarded; submission is not Reset
    void overwritten_blas(ResourceKey allocation,std::uint64_t native_blas);
    void written(ResourceKey,std::uint64_t offset,std::uint64_t bytes);
    void destroyed(ResourceKey);
    Frozen freeze(Recording) const; // actual pre-Execute snapshot
    // Call only after native Execute and a successful Signal on that actual queue.
    void submitted(const Frozen &,NativeIdentityFence);
    NativeIdentityFence producer_of(const Frozen &,const std::shared_ptr<const NativeBlasIdentity> &) const;
    // Value must come from this exact fence's GetCompletedValue, never planned Signal.
    void completion(std::uint64_t fence,std::uint64_t value);
    std::uint64_t validity_revision()const noexcept{return validity_revision_;}
    std::optional<Match> resolve(const DrawIdentityQuery &,LookupReceipt *receipt=nullptr) const;
    // Author inputs are owned CPU data. Demand a unique exact source and an
    // actual recording/submission receipt, but do not require GPU completion.
    std::optional<Match> resolve_snapshot(const DrawIdentityQuery &,LookupReceipt &) const;
    // Negative prefilter only; does not prove a resource, pose or queue order.
    bool might_resolve(const DrawIdentityQuery &)const;
    bool live(const std::shared_ptr<const NativeBlasIdentity> &,unsigned component) const;
    bool live_state(const std::shared_ptr<const NativeBlasIdentity> &,unsigned &submissions_out,NativeIdentityFence &producer_out) const;
};
}
