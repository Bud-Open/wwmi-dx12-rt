#pragma once
// A deliberately finite DXIL program model. No D3D, game or filesystem access.
// Call only on shader/PSO creation; draws consume immutable results.
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wwmi::program {
inline constexpr uint32_t parser_version=2;
inline constexpr uint32_t rules_version=2;
enum class Stage : uint8_t { vertex, pixel };
enum class Recognition : uint8_t { unsupported, executable_model, native_pixel_interface };
enum class Adaptation : uint8_t { unrecognized, executable_equivalent, layout_instance, native_material };
struct Layout {
    uint32_t view_reg=0, primitive_reg=1, local_reg=2;
    uint32_t view_mode_row=308; // uint4 row, not a byte offset
    uint32_t view_gate_offset=2560, primitive_gate_offset=428, primitive_gate_mask=1;
    uint32_t view_declared_bytes=0, primitive_declared_bytes=0;
    uint32_t view_read_end=0, primitive_read_end=0;
    uint32_t view_snapshot_bytes=5952, primitive_snapshot_bytes=720;
    uint32_t view_mode_offset() const { return 16u*view_mode_row; }
    bool snapshot_covers_reads() const {
        return view_read_end<=view_snapshot_bytes && primitive_read_end<=primitive_snapshot_bytes;
    }
};
struct Signature {
    uint32_t id=0, component=0, kind=0, interpolation=0, rows=0, columns=0;
    int32_t start_row=0,start_column=0; // Native depth output uses -1/-1, not a color register.
    std::string name;
    std::vector<uint32_t> indices;
};
struct Resource {
    uint32_t resource_class=0, range_id=0, space=0, reg=0, count=0, bytes=0;
    std::string type;
};
struct Facts {
    uint32_t parser=parser_version, rules=rules_version;
    uint64_t native_identity=0; // Always the actual container identity; never a model alias.
    Stage stage=Stage::vertex;
    Recognition recognition=Recognition::unsupported;
    Adaptation adaptation=Adaptation::unrecognized;
    std::string reason, model;
    struct Difference {
        std::string nearest_model,expected,observed;
        uint32_t canonical_line=0;
    } difference;
    Layout layout;
    int uv=-1, tangent=-1, auxiliary=-1, previous=-1;
    bool auxiliary_requires_native_color=false;
    bool previous_only_velocity=false, pixel_ignores_previous=false;
    // Native invalid-velocity proof: VS previous=(0,0,0,1), PS Target4=(0,0,0,0).
    // Each stage proves its own convention; this is not a shared all-zero rule.
    bool native_gate_zero_velocity=false;
    std::array<uint8_t,4> native_gate_closed_output{};
    bool native_depth_less_equal=false;
    bool all_resource_reads_modelled=false;
    uint32_t color_target_count=0;
    std::array<uint64_t,2> pixel_srv_reads{};
    std::map<uint32_t,uint32_t> cb_read_end;
    std::vector<uint32_t> dynamic_cb_registers;
    std::vector<Signature> inputs, outputs;
    std::vector<Resource> resources;
};
struct Pair {
    bool current_previous=false, no_history=false, pixel_unused_previous=false;
    int pixel_primitive=-1;
    std::string reason;
};
// The supplied text must come from DXC Disassemble of the actual container.
Facts analyse_ir(const std::string&,uint64_t native_identity,Stage);
Pair pair(const Facts& vertex,const Facts& pixel,uint32_t rtv_count);
using Disassemble=std::function<bool(const void*,size_t,std::string&,std::string&)>;
struct CacheStats { uint64_t parses=0,hits=0,entries=0; };
class ContentCache {
public:
    std::shared_ptr<const Facts> resolve(const void*,size_t,uint64_t native_identity,Stage,const Disassemble&);
    CacheStats stats() const;
private:
    struct Entry {
        uint32_t parser=parser_version,rules=rules_version;
        Stage stage=Stage::vertex;
        std::vector<unsigned char> bytes;
        std::shared_ptr<const Facts> facts;
    };
    mutable std::mutex mutex_;
    std::map<uint64_t,std::vector<Entry>> entries_;
    CacheStats stats_;
};
}
