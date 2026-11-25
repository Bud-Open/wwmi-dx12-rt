// SPDX-License-Identifier: GPL-3.0-only
// This bridge observes CPU API boundaries. It does not issue rendering commands,
// read GPU memory, change an argument, or own any game COM reference.
#define WWMI_NATIVE_TRACE_BUILD
#include "wwmi_native_trace.hpp"
#include "wwmi_native_resource_ids.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using wwmi::native_trace::CallToken;

struct ListIdentity {
    uint64_t creation_id = 0;
    uint64_t generation = 0;
    const void *wrapper = nullptr;
    const void *native = nullptr;
    uint32_t type = 0;
};

struct Bridge {
    std::mutex subscription_mutex;
    std::condition_variable drained;
    WWMI_NativeTrace_Callback_v1 callback = nullptr;
    void *user = nullptr;
    uint64_t active_session = 0;
    uint64_t session_counter = 0;
    size_t in_flight = 0;
    bool closing = false;
    std::atomic<uint64_t> session { 0 };
    std::atomic<uint64_t> seq { 0 };
    std::atomic<uint64_t> calls { 0 };
    std::atomic<uint64_t> identities { 0 };
    std::atomic<uint64_t> trace_failures { 0 };
    std::atomic<uint64_t> callback_failures { 0 };
    std::atomic<uint64_t> recursive_events_suppressed { 0 };
    std::mutex identity_mutex;
    std::unordered_map<const void *, ListIdentity> lists;
    std::unordered_map<const void *, uint64_t> resources;
};

// Intentionally process-lifetime storage: no static destructor races with late
// D3D12 object destruction during host DLL shutdown.
Bridge &bridge() {
    static Bridge *const value = new Bridge;
    return *value;
}

thread_local unsigned callback_depth = 0;
thread_local std::vector<CallToken> state_object_requests;

std::string hex64(uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    char result[19] = "0x0000000000000000";
    for (unsigned i = 0; i != 16; ++i) {
        result[17 - i] = digits[value & 15u];
        value >>= 4;
    }
    return std::string(result, 18);
}

std::string quoted(const char *text) {
    std::string out = "\"";
    if (text) {
        for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
            switch (*p) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (*p < 0x20) {
                    static constexpr char d[] = "0123456789abcdef";
                    out += "\\u00";
                    out += d[*p >> 4]; out += d[*p & 15];
                } else out += static_cast<char>(*p);
            }
        }
    }
    out += '"';
    return out;
}

struct Json {
    std::string data = "{";
    bool first = true;
    void raw(const char *key, const std::string &value) {
        if (!first) data += ',';
        first = false;
        data += quoted(key); data += ':'; data += value;
    }
    void text(const char *key, const char *value) { raw(key, quoted(value)); }
    void text(const char *key, const std::string &value) { raw(key, quoted(value.c_str())); }
    void u32(const char *key, uint32_t value) { raw(key, std::to_string(value)); }
    void hex(const char *key, uint64_t value) { raw(key, quoted(hex64(value).c_str())); }
    void pointer(const char *key, const void *value) { hex(key, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value))); }
    void boolean(const char *key, bool value) { raw(key, value ? "true" : "false"); }
    std::string finish() { data += '}'; return std::move(data); }
};

// All inputs to this helper are documented CPU pointers, never GPU virtual
// addresses. ReadProcessMemory prevents a malformed CPU descriptor from causing
// the observer to fault before the original API can handle that application call.
template <typename T>
bool cpu_read(const T *address, T &out) noexcept {
    if (!address) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, &out, sizeof(T), &copied) != FALSE && copied == sizeof(T);
}

template <typename T>
bool cpu_index(const T *base, uint64_t index, T &out) noexcept {
    if (!base || index > (std::numeric_limits<uintptr_t>::max)() / sizeof(T)) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(base);
    const uintptr_t delta = static_cast<uintptr_t>(index) * sizeof(T);
    if (start > (std::numeric_limits<uintptr_t>::max)() - delta) return false;
    return cpu_read(reinterpret_cast<const T *>(start + delta), out);
}

// JSON unicode escapes preserve UTF-16 shader names without locale conversion.
// Bounds are an explicit evidence limit, not an implicit successful truncation.
std::string wide_json(LPCWSTR address, bool &complete) {
    if (!address) return "null";
    static constexpr char digits[] = "0123456789abcdef";
    std::string result = "\"";
    constexpr uint64_t max_units = 65536;
    for (uint64_t i = 0; i < max_units; ++i) {
        WCHAR ch = 0;
        if (!cpu_index(address, i, ch)) { complete = false; result += '"'; return result; }
        if (ch == 0) { result += '"'; return result; }
        if (ch >= 0x20 && ch <= 0x7e && ch != '"' && ch != '\\') {
            result += static_cast<char>(ch);
        } else {
            result += "\\u";
            const uint16_t value = static_cast<uint16_t>(ch);
            result += digits[(value >> 12) & 15]; result += digits[(value >> 8) & 15];
            result += digits[(value >> 4) & 15]; result += digits[value & 15];
        }
    }
    complete = false;
    result += '"';
    return result;
}

bool callback_cpp(WWMI_NativeTrace_Callback_v1 callback, const char *record,
    size_t length, void *user) noexcept {
    try { callback(record, length, user); return true; }
    catch (...) { return false; }
}

bool callback_isolated(WWMI_NativeTrace_Callback_v1 callback, const char *record,
    size_t length, void *user) noexcept {
#if defined(_MSC_VER)
    __try { return callback_cpp(callback, record, length, user); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
#else
    // Portable C++ exception isolation. Access violations in a foreign callback
    // are not claimed to be isolated by non-MSVC builds.
    return callback_cpp(callback, record, length, user);
#endif
}

void deliver(uint64_t session, const std::string &record) noexcept {
    try {
        Bridge &b = bridge();
        if (callback_depth != 0) { ++b.recursive_events_suppressed; return; }
        WWMI_NativeTrace_Callback_v1 callback = nullptr;
        void *user = nullptr;
        {
            std::lock_guard<std::mutex> lock(b.subscription_mutex);
            if (!session || b.closing || !b.callback || b.active_session != session) return;
            callback = b.callback; user = b.user; ++b.in_flight;
        }
        ++callback_depth;
        const bool ok = callback_isolated(callback, record.data(), record.size(), user);
        --callback_depth;
        if (!ok) ++b.callback_failures;
        {
            std::lock_guard<std::mutex> lock(b.subscription_mutex);
            --b.in_flight;
            if (b.in_flight == 0) b.drained.notify_all();
        }
    } catch (...) {
        // No observation exception may escape into a rendering API wrapper.
    }
}

CallToken new_call() noexcept {
    try {
        Bridge &b = bridge();
        if (callback_depth != 0) { ++b.recursive_events_suppressed; return {}; }
        const uint64_t session = b.session.load(std::memory_order_acquire);
        if (!session) return {};
        return { ++b.calls, session };
    } catch (...) { return {}; }
}

Json event(CallToken token, const char *kind, const char *phase) {
    Bridge &b = bridge();
    LARGE_INTEGER now {}, frequency {};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
    Json out;
    out.text("schema", "wwmi.native.v1"); out.text("kind", kind); out.text("phase", phase);
    out.hex("seq", ++b.seq); out.hex("call_id", token.call_id); out.hex("session", token.session);
    out.hex("thread_id", GetCurrentThreadId()); out.hex("process_id", GetCurrentProcessId());
    out.hex("qpc", static_cast<uint64_t>(now.QuadPart)); out.hex("qpc_frequency", static_cast<uint64_t>(frequency.QuadPart));
    out.hex("trace_failures_total", b.trace_failures.load());
    out.hex("callback_failures_total", b.callback_failures.load());
    out.hex("recursive_events_suppressed_total", b.recursive_events_suppressed.load());
    return out;
}

void send(CallToken token, Json &record) { deliver(token.session, record.finish()); }

void failure(CallToken token, const char *context) noexcept {
    try {
        ++bridge().trace_failures;
        if (!token.session) return;
        Json out = event(token, "trace_error", "observation");
        out.text("context", context); out.boolean("capture_complete", false);
        out.boolean("original_call_changed_by_bridge", false);
        send(token, out);
    } catch (...) {}
}

void list_fields(Json &out, const void *wrapper, const void *native) {
    out.pointer("command_list", wrapper); out.pointer("native_command_list", native);
    Bridge &b = bridge();
    std::lock_guard<std::mutex> lock(b.identity_mutex);
    auto it = b.lists.find(wrapper);
    if (it == b.lists.end()) it = b.lists.find(native);
    out.boolean("list_identity_known", it != b.lists.end());
    if (it != b.lists.end()) {
        out.hex("list_creation_id", it->second.creation_id);
        out.hex("recording_generation", it->second.generation);
        out.u32("command_list_type", it->second.type);
    }
}

void table_fields(Json &out, const char *key, D3D12_GPU_VIRTUAL_ADDRESS address,
    uint64_t size, uint64_t stride, bool has_stride) {
    Json table;
    table.hex("start_address", address); table.hex("size_in_bytes", size);
    if (has_stride) table.hex("stride_in_bytes", stride);
    table.boolean("gpu_bytes_read", false);
    out.raw(key, table.finish());
}

bool export_records(CallToken token, UINT subobject_index, UINT count,
    const D3D12_EXPORT_DESC *exports) {
    bool complete = true;
    for (UINT i = 0; i < count; ++i) {
        D3D12_EXPORT_DESC item {};
        const bool readable = cpu_index(exports, i, item);
        Json out = event(token, "state_object_export", "detail");
        out.u32("subobject_index", subobject_index); out.u32("export_index", i);
        out.boolean("cpu_read_ok", readable);
        if (readable) {
            bool strings_ok = true;
            out.raw("name", wide_json(item.Name, strings_ok));
            out.raw("export_to_rename", wide_json(item.ExportToRename, strings_ok));
            out.u32("flags", static_cast<uint32_t>(item.Flags));
            out.boolean("strings_complete", strings_ok); complete = complete && strings_ok;
        } else complete = false;
        send(token, out);
        if (!readable) break;
    }
    return complete;
}

bool association_exports(CallToken token, UINT subobject_index, UINT count,
    LPCWSTR const *exports) {
    bool complete = true;
    for (UINT i = 0; i < count; ++i) {
        LPCWSTR item = nullptr;
        const bool readable = cpu_index(exports, i, item);
        Json out = event(token, "state_object_association_export", "detail");
        out.u32("subobject_index", subobject_index); out.u32("export_index", i);
        out.boolean("cpu_read_ok", readable);
        if (readable) {
            bool strings_ok = true; out.raw("name", wide_json(item, strings_ok));
            out.boolean("strings_complete", strings_ok); complete = complete && strings_ok;
        } else complete = false;
        send(token, out);
        if (!readable) break;
    }
    return complete;
}

} // anonymous namespace

extern "C" int32_t WWMI_NATIVE_CALL WWMI_NativeTrace_Register_v1(
    WWMI_NativeTrace_Callback_v1 callback, void *user) {
    if (!callback) return -1;
    if (callback_depth) return -3;
    bool registered = false;
    uint64_t registered_session = 0;
    try {
        Bridge &b = bridge();
        uint64_t session = 0;
        {
            std::lock_guard<std::mutex> lock(b.subscription_mutex);
            if (b.closing) return -2;
            if (b.callback) return (b.callback == callback && b.user == user) ? 0 : -2;
            b.callback = callback; b.user = user;
            session = b.active_session = ++b.session_counter;
            b.session.store(session, std::memory_order_release);
            registered = true;
            registered_session = session;
        }
        const CallToken token { 0, session };
        Json out = event(token, "bridge_session", "begin");
        out.text("bridge_version", "stage1-0.1"); out.text("reshade_commit", "05c95f4ac083d1c4c1ab846ffe65bf4b775d3bf9");
        out.boolean("observer_only", true); out.boolean("gpu_bytes_read", false);
        out.text("snapshot_encoding", "header+indexed_details+snapshot_complete");
        out.text("coverage", "BuildAS,DispatchRays,StateObject,ExecuteCommandLists,QueueWaitSignal,Reset,resource_basic,CopyAS,Postbuild,SetPipelineState1");
        out.text("not_captured", "GPU_payloads,root_arguments,descriptor_contents,barriers,ExecuteIndirect,vendor_calls,fence_completion,present,skin_identity");
        send(token, out);
        return 0;
    } catch (...) {
        // An optional header serialization failure must not report registration
        // failure after publishing a live callback that the caller might unload.
        if (registered) { failure({ 0, registered_session }, "Session header serialization exception"); return 0; }
        return -2;
    }
}

extern "C" int32_t WWMI_NATIVE_CALL WWMI_NativeTrace_Unregister_v1(
    WWMI_NativeTrace_Callback_v1 callback, void *user) {
    if (!callback) return -1;
    if (callback_depth) return -3;
    try {
        Bridge &b = bridge();
        std::unique_lock<std::mutex> lock(b.subscription_mutex);
        if (b.closing) return -2;
        if (b.callback != callback || b.user != user) return -4;
        b.closing = true;
        b.session.store(0, std::memory_order_release);
        b.drained.wait(lock, [&b] { return b.in_flight == 0; });
        b.callback = nullptr; b.user = nullptr; b.active_session = 0; b.closing = false;
        return 0;
    } catch (...) { return -2; }
}

namespace wwmi::native_trace {

bool enabled() noexcept {
    try { return callback_depth == 0 && bridge().session.load(std::memory_order_acquire) != 0; }
    catch (...) { return false; }
}

CallToken build_as_before(const void *command_list, const void *native_command_list,
    const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc,
    UINT postbuild_count,
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *postbuild) noexcept {
    const CallToken token = new_call();
    if (!token.session) return token;
    try {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC value {};
        const bool readable = cpu_read(desc, value);
        Json head = event(token, "build_as", "before");
        list_fields(head, command_list, native_command_list);
        head.pointer("cpu_desc", desc); head.boolean("cpu_read_ok", readable);
        head.u32("postbuild_count", postbuild_count); head.pointer("cpu_postbuild_descs", postbuild);
        head.boolean("gpu_bytes_read", false);
        if (!readable) { send(token, head); failure(token, "BuildAS descriptor unreadable"); return token; }
        head.hex("dest_as", value.DestAccelerationStructureData);
        head.hex("source_as", value.SourceAccelerationStructureData);
        head.hex("scratch", value.ScratchAccelerationStructureData);
        head.u32("as_type", static_cast<uint32_t>(value.Inputs.Type));
        head.u32("flags", static_cast<uint32_t>(value.Inputs.Flags));
        head.u32("num_descs", value.Inputs.NumDescs);
        head.u32("descs_layout", static_cast<uint32_t>(value.Inputs.DescsLayout));
        if (value.Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
            head.hex("instance_descs_gpu_va", value.Inputs.InstanceDescs);
            head.text("instance_bytes", "not_read_gpu_address");
        } else head.pointer("cpu_geometry_array", value.Inputs.pGeometryDescs);
        send(token, head);
        bool complete = true;
        uint32_t geometries = 0, postbuilds = 0;
        if (value.Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) {
            for (UINT i = 0; i < value.Inputs.NumDescs; ++i) {
                D3D12_RAYTRACING_GEOMETRY_DESC geometry {};
                bool geometry_ok = false;
                const void *cpu_geometry = nullptr;
                if (value.Inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY) {
                    geometry_ok = cpu_index(value.Inputs.pGeometryDescs, i, geometry);
                    cpu_geometry = reinterpret_cast<const void *>(reinterpret_cast<uintptr_t>(value.Inputs.pGeometryDescs) + static_cast<uintptr_t>(i) * sizeof(geometry));
                } else if (value.Inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS) {
                    const D3D12_RAYTRACING_GEOMETRY_DESC *ptr = nullptr;
                    geometry_ok = cpu_index(value.Inputs.ppGeometryDescs, i, ptr) && cpu_read(ptr, geometry);
                    cpu_geometry = ptr;
                }
                Json item = event(token, "build_as_geometry", "detail");
                item.u32("geometry_index", i); item.pointer("cpu_geometry", cpu_geometry);
                item.boolean("cpu_read_ok", geometry_ok);
                if (geometry_ok) {
                    item.u32("geometry_type", static_cast<uint32_t>(geometry.Type));
                    item.u32("flags", static_cast<uint32_t>(geometry.Flags));
                    if (geometry.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES) {
                        const auto &t = geometry.Triangles;
                        item.hex("transform_3x4_gpu_va", t.Transform3x4);
                        item.u32("index_format", static_cast<uint32_t>(t.IndexFormat));
                        item.u32("vertex_format", static_cast<uint32_t>(t.VertexFormat));
                        item.u32("index_count", t.IndexCount); item.u32("vertex_count", t.VertexCount);
                        item.hex("index_buffer_gpu_va", t.IndexBuffer);
                        item.hex("vertex_buffer_gpu_va", t.VertexBuffer.StartAddress);
                        item.hex("vertex_stride_bytes", t.VertexBuffer.StrideInBytes);
                        item.boolean("opaque_payload", false);
                    } else if (geometry.Type == D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS) {
                        item.hex("aabb_count", geometry.AABBs.AABBCount);
                        item.hex("aabb_gpu_va", geometry.AABBs.AABBs.StartAddress);
                        item.hex("aabb_stride_bytes", geometry.AABBs.AABBs.StrideInBytes);
                        item.boolean("opaque_payload", false);
                    } else {
                        item.boolean("opaque_payload", true); complete = false;
                    }
                    ++geometries;
                } else complete = false;
                item.boolean("gpu_bytes_read", false); send(token, item);
                if (!geometry_ok) break;
            }
        } else if (value.Inputs.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) complete = false;
        for (UINT i = 0; i < postbuild_count; ++i) {
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC item_value {};
            const bool item_ok = cpu_index(postbuild, i, item_value);
            Json item = event(token, "build_as_postbuild", "detail");
            item.u32("postbuild_index", i); item.boolean("cpu_read_ok", item_ok);
            if (item_ok) {
                item.hex("dest_buffer_gpu_va", item_value.DestBuffer);
                item.u32("info_type", static_cast<uint32_t>(item_value.InfoType)); ++postbuilds;
            } else complete = false;
            send(token, item); if (!item_ok) break;
        }
        Json end = event(token, "build_as_snapshot", "before");
        end.u32("geometries_captured", geometries); end.u32("postbuilds_captured", postbuilds);
        end.boolean("capture_complete", complete); end.boolean("gpu_bytes_read", false);
        send(token, end);
    } catch (...) { failure(token, "BuildAS serialization exception"); }
    return token;
}

CallToken dispatch_rays_before(const void *command_list, const void *native_command_list,
    const D3D12_DISPATCH_RAYS_DESC *desc) noexcept {
    const CallToken token = new_call(); if (!token.session) return token;
    try {
        D3D12_DISPATCH_RAYS_DESC value {};
        const bool readable = cpu_read(desc, value);
        Json out = event(token, "dispatch_rays", "before"); list_fields(out, command_list, native_command_list);
        out.pointer("cpu_desc", desc); out.boolean("capture_complete", readable);
        if (readable) {
            table_fields(out, "ray_generation", value.RayGenerationShaderRecord.StartAddress, value.RayGenerationShaderRecord.SizeInBytes, 0, false);
            table_fields(out, "miss", value.MissShaderTable.StartAddress, value.MissShaderTable.SizeInBytes, value.MissShaderTable.StrideInBytes, true);
            table_fields(out, "hit_group", value.HitGroupTable.StartAddress, value.HitGroupTable.SizeInBytes, value.HitGroupTable.StrideInBytes, true);
            table_fields(out, "callable", value.CallableShaderTable.StartAddress, value.CallableShaderTable.SizeInBytes, value.CallableShaderTable.StrideInBytes, true);
            out.u32("width", value.Width); out.u32("height", value.Height); out.u32("depth", value.Depth);
        }
        send(token, out);
    } catch (...) { failure(token, "DispatchRays serialization exception"); }
    return token;
}

CallToken state_object_before(const void *device, const void *native_device,
    const char *api, const char *path, const D3D12_STATE_OBJECT_DESC *desc,
    const void *existing_collection) noexcept {
    const CallToken token = new_call(); if (!token.session) return token;
    try {
        const bool wrapper_input = path && std::strcmp(path, "wrapper_input") == 0;
        const uint64_t parent = state_object_requests.empty() ? 0 : state_object_requests.back().call_id;
        if (wrapper_input) state_object_requests.push_back(token);
        D3D12_STATE_OBJECT_DESC value {};
        const bool readable = cpu_read(desc, value);
        Json head = event(token, "state_object", "before");
        head.pointer("device", device); head.pointer("native_device", native_device);
        head.text("api", api); head.text("path", path); head.hex("parent_wrapper_call_id", parent);
        head.pointer("existing_state_object", existing_collection); head.pointer("cpu_desc", desc);
        head.boolean("cpu_read_ok", readable);
        if (!readable) { send(token, head); failure(token, "StateObject descriptor unreadable"); return token; }
        head.u32("state_object_type", static_cast<uint32_t>(value.Type)); head.u32("num_subobjects", value.NumSubobjects);
        head.pointer("cpu_subobjects", value.pSubobjects); send(token, head);
        bool complete = true; uint32_t captured = 0;
        for (UINT i = 0; i < value.NumSubobjects; ++i) {
            D3D12_STATE_SUBOBJECT sub {};
            const bool sub_ok = cpu_index(value.pSubobjects, i, sub);
            Json out = event(token, "state_object_subobject", "detail");
            out.u32("subobject_index", i); out.boolean("cpu_read_ok", sub_ok);
            if (!sub_ok) { complete = false; send(token, out); break; }
            out.u32("native_type", static_cast<uint32_t>(sub.Type)); out.pointer("cpu_payload", sub.pDesc);
            bool known = true, payload_ok = true;
            UINT export_count = 0; const D3D12_EXPORT_DESC *exports = nullptr;
            UINT association_count = 0; LPCWSTR const *names = nullptr;
            switch (sub.Type) {
            case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG: {
                D3D12_STATE_OBJECT_CONFIG p {}; payload_ok = cpu_read(static_cast<const D3D12_STATE_OBJECT_CONFIG *>(sub.pDesc), p);
                if (payload_ok) out.u32("flags", static_cast<uint32_t>(p.Flags)); break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE: {
                D3D12_GLOBAL_ROOT_SIGNATURE p {}; payload_ok = cpu_read(static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE *>(sub.pDesc), p);
                if (payload_ok) out.pointer("root_signature", p.pGlobalRootSignature); break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE: {
                D3D12_LOCAL_ROOT_SIGNATURE p {}; payload_ok = cpu_read(static_cast<const D3D12_LOCAL_ROOT_SIGNATURE *>(sub.pDesc), p);
                if (payload_ok) out.pointer("root_signature", p.pLocalRootSignature); break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK: {
                D3D12_NODE_MASK p {}; payload_ok = cpu_read(static_cast<const D3D12_NODE_MASK *>(sub.pDesc), p);
                if (payload_ok) out.u32("node_mask", p.NodeMask); break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: {
                D3D12_DXIL_LIBRARY_DESC p {}; payload_ok = cpu_read(static_cast<const D3D12_DXIL_LIBRARY_DESC *>(sub.pDesc), p);
                if (payload_ok) {
                    out.pointer("cpu_bytecode", p.DXILLibrary.pShaderBytecode); out.hex("bytecode_length", p.DXILLibrary.BytecodeLength);
                    out.boolean("bytecode_bytes_read", false); out.u32("num_exports", p.NumExports);
                    export_count = p.NumExports; exports = p.pExports;
                } break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION: {
                D3D12_EXISTING_COLLECTION_DESC p {}; payload_ok = cpu_read(static_cast<const D3D12_EXISTING_COLLECTION_DESC *>(sub.pDesc), p);
                if (payload_ok) { out.pointer("existing_collection", p.pExistingCollection); out.u32("num_exports", p.NumExports); export_count = p.NumExports; exports = p.pExports; }
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION: {
                D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION p {}; payload_ok = cpu_read(static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION *>(sub.pDesc), p);
                if (payload_ok) {
                    out.pointer("associated_subobject", p.pSubobjectToAssociate);
                    const uintptr_t base = reinterpret_cast<uintptr_t>(value.pSubobjects);
                    const uintptr_t target = reinterpret_cast<uintptr_t>(p.pSubobjectToAssociate);
                    const uint64_t bytes = static_cast<uint64_t>(value.NumSubobjects) * sizeof(D3D12_STATE_SUBOBJECT);
                    const bool in_array = target >= base && static_cast<uint64_t>(target - base) < bytes && (target - base) % sizeof(D3D12_STATE_SUBOBJECT) == 0;
                    out.boolean("association_target_in_array", in_array);
                    if (in_array) out.u32("associated_subobject_index", static_cast<uint32_t>((target - base) / sizeof(D3D12_STATE_SUBOBJECT)));
                    else complete = false;
                    out.u32("num_exports", p.NumExports); association_count = p.NumExports; names = p.pExports;
                } break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION: {
                D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION p {}; payload_ok = cpu_read(static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION *>(sub.pDesc), p);
                if (payload_ok) { out.raw("subobject_to_associate", wide_json(p.SubobjectToAssociate, payload_ok)); out.u32("num_exports", p.NumExports); association_count = p.NumExports; names = p.pExports; }
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG: {
                D3D12_RAYTRACING_SHADER_CONFIG p {}; payload_ok = cpu_read(static_cast<const D3D12_RAYTRACING_SHADER_CONFIG *>(sub.pDesc), p);
                if (payload_ok) { out.u32("max_payload_size_bytes", p.MaxPayloadSizeInBytes); out.u32("max_attribute_size_bytes", p.MaxAttributeSizeInBytes); } break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG: {
                D3D12_RAYTRACING_PIPELINE_CONFIG p {}; payload_ok = cpu_read(static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG *>(sub.pDesc), p);
                if (payload_ok) out.u32("max_trace_recursion_depth", p.MaxTraceRecursionDepth); break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1: {
                D3D12_RAYTRACING_PIPELINE_CONFIG1 p {}; payload_ok = cpu_read(static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG1 *>(sub.pDesc), p);
                if (payload_ok) { out.u32("max_trace_recursion_depth", p.MaxTraceRecursionDepth); out.u32("flags", static_cast<uint32_t>(p.Flags)); } break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP: {
                D3D12_HIT_GROUP_DESC p {}; payload_ok = cpu_read(static_cast<const D3D12_HIT_GROUP_DESC *>(sub.pDesc), p);
                if (payload_ok) {
                    out.u32("hit_group_type", static_cast<uint32_t>(p.Type));
                    out.raw("hit_group_export", wide_json(p.HitGroupExport, payload_ok));
                    out.raw("any_hit_shader_import", wide_json(p.AnyHitShaderImport, payload_ok));
                    out.raw("closest_hit_shader_import", wide_json(p.ClosestHitShaderImport, payload_ok));
                    out.raw("intersection_shader_import", wide_json(p.IntersectionShaderImport, payload_ok));
                } break;
            }
            default: known = false; break;
            }
            out.boolean("opaque_payload", !known); out.boolean("payload_read_ok", known && payload_ok);
            out.boolean("gpu_bytes_read", false); send(token, out); ++captured;
            complete = complete && known && payload_ok;
            if (payload_ok && known) {
                const bool exports_ok = export_records(token, i, export_count, exports);
                const bool association_ok = association_exports(token, i, association_count, names);
                complete = complete && exports_ok && association_ok;
            }
        }
        Json end = event(token, "state_object_snapshot", "before");
        end.u32("subobjects_captured", captured); end.boolean("capture_complete", complete);
        end.boolean("shader_bytecode_bytes_read", false); end.boolean("root_signature_bytes_read", false);
        send(token, end);
    } catch (...) { failure(token, "StateObject serialization exception"); }
    return token;
}

CallToken execute_before(const void *queue, const void *native_queue, UINT count,
    ID3D12CommandList *const *caller_lists, ID3D12CommandList *const *native_lists) noexcept {
    const CallToken token = new_call(); if (!token.session) return token;
    try {
        Json head = event(token, "queue_execute", "before"); head.pointer("queue", queue); head.pointer("native_queue", native_queue);
        head.u32("num_command_lists", count); head.boolean("gpu_completed", false); send(token, head);
        bool complete = true; uint32_t captured = 0;
        for (UINT i = 0; i < count; ++i) {
            ID3D12CommandList *caller = nullptr, *native = nullptr;
            const bool ok = cpu_index(caller_lists, i, caller) && cpu_index(native_lists, i, native);
            Json out = event(token, "queue_execute_list", "detail"); out.pointer("queue", queue); out.u32("list_index", i);
            out.boolean("cpu_read_ok", ok); if (ok) { list_fields(out, caller, native); ++captured; } else complete = false;
            send(token, out); if (!ok) break;
        }
        Json end = event(token, "queue_execute_snapshot", "before"); end.u32("lists_captured", captured);
        end.boolean("capture_complete", complete); send(token, end);
    } catch (...) { failure(token, "ExecuteCommandLists serialization exception"); }
    return token;
}

CallToken queue_sync_before(const char *kind, const void *queue,
    const void *native_queue, const void *fence, uint64_t value) noexcept {
    const CallToken token = new_call(); if (!token.session) return token;
    try {
        Json out = event(token, kind, "before"); out.pointer("queue", queue); out.pointer("native_queue", native_queue);
        out.pointer("fence", fence); out.hex("value", value); out.boolean("gpu_completed", false);
        out.text("meaning", "queue_operation_request_not_fence_completion"); send(token, out);
    } catch (...) { failure(token, "Queue sync serialization exception"); }
    return token;
}

void call_after(CallToken token, const char *kind, const void *object,
    bool has_hresult, HRESULT hr, const void *result, const char *outcome) noexcept {
    if (!token.session) return;
    try {
        if (kind && std::strcmp(kind, "state_object") == 0 && !state_object_requests.empty() && state_object_requests.back().call_id == token.call_id)
            state_object_requests.pop_back();
        Json out = event(token, kind, "after"); out.pointer("object", object); out.text("outcome", outcome);
        out.boolean("cpu_api_returned", std::strcmp(outcome, "returned") == 0);
        out.boolean("gpu_completed", false); out.boolean("has_hresult", has_hresult);
        if (has_hresult) { out.hex("hresult", static_cast<uint32_t>(hr)); out.boolean("succeeded", SUCCEEDED(hr)); }
        if (result) out.pointer("result", result);
        send(token, out);
    } catch (...) { failure(token, "After-call serialization exception"); }
}

void command_list_created(const void *command_list, ID3D12GraphicsCommandList *native_command_list) noexcept {
    CallToken token {};
    try {
        Bridge &b = bridge();
        ListIdentity identity { ++b.identities, 0, command_list, native_command_list, static_cast<uint32_t>(native_command_list->GetType()) };
        { std::lock_guard<std::mutex> lock(b.identity_mutex); b.lists[command_list] = identity; b.lists[native_command_list] = identity; }
        token = new_call(); if (!token.session) return;
        Json out = event(token, "command_list_create", "after"); list_fields(out, command_list, native_command_list);
        out.text("initial_recording_state", "not_inferred_from_wrapper_constructor"); send(token, out);
    } catch (...) { failure(token, "Command list create observation exception"); }
}

void command_list_destroyed(const void *command_list, const void *native_command_list) noexcept {
    CallToken token {};
    try {
        token = new_call();
        if (token.session) { Json out = event(token, "command_list_destroy", "wrapper_destruction"); list_fields(out, command_list, native_command_list); send(token, out); }
        Bridge &b = bridge(); std::lock_guard<std::mutex> lock(b.identity_mutex);
        b.lists.erase(command_list); b.lists.erase(native_command_list);
    } catch (...) { failure(token, "Command list destruction observation exception"); }
}

void command_list_reset_after(const void *command_list, const void *native_command_list,
    const void *allocator, const void *initial_pipeline, HRESULT hr) noexcept {
    CallToken token {};
    try {
        Bridge &b = bridge();
        bool generation_advanced = false;
        if (SUCCEEDED(hr)) {
            std::lock_guard<std::mutex> lock(b.identity_mutex);
            const auto it = b.lists.find(command_list);
            if (it != b.lists.end()) {
                ListIdentity id = it->second; ++id.generation;
                b.lists[id.wrapper] = id; b.lists[id.native] = id;
                generation_advanced = true;
            }
        }
        token = new_call(); if (!token.session) return;
        Json out = event(token, "command_list_reset", "after"); list_fields(out, command_list, native_command_list);
        out.pointer("allocator", allocator); out.pointer("initial_pipeline", initial_pipeline);
        out.hex("hresult", static_cast<uint32_t>(hr)); out.boolean("succeeded", SUCCEEDED(hr));
        out.boolean("generation_advanced", generation_advanced); send(token, out);
    } catch (...) { failure(token, "Command list Reset observation exception"); }
}

void command_list_close_after(const void *command_list, const void *native_command_list, HRESULT hr) noexcept {
    const CallToken token = new_call(); if (!token.session) return;
    try {
        Json out = event(token, "command_list_close", "after"); list_fields(out, command_list, native_command_list);
        out.hex("hresult", static_cast<uint32_t>(hr)); out.boolean("succeeded", SUCCEEDED(hr)); send(token, out);
    } catch (...) { failure(token, "Command list Close observation exception"); }
}

void pipeline_state_bound(const void *command_list, const void *native_command_list, const void *state_object) noexcept {
    const CallToken token = new_call(); if (!token.session) return;
    try {
        Json out = event(token, "set_pipeline_state1", "after"); list_fields(out, command_list, native_command_list);
        out.pointer("state_object", state_object); send(token, out);
    } catch (...) { failure(token, "SetPipelineState1 observation exception"); }
}

ResourceGenerations resource_generations(const void *resource, const void *counter) noexcept {
    try {
        Bridge &b = bridge();
        const std::lock_guard<std::mutex> lock(b.identity_mutex);
        const auto lookup = [&](const void *object) -> uint64_t {
            if (const auto it = b.resources.find(object); it != b.resources.end())
                return it->second;
            return 0;
        };
        return { lookup(resource), counter ? lookup(counter) : 0 };
    } catch (...) {
        return {};
    }
}

uint64_t resource_created(const void *device, ID3D12Resource *resource, uint64_t initial_usage) noexcept {
    CallToken token {}; uint64_t creation_id = 0;
    try {
        if (!resource) return 0;
        Bridge &b = bridge(); creation_id = ++b.identities;
        { std::lock_guard<std::mutex> lock(b.identity_mutex); b.resources[resource] = creation_id; }
        token = new_call(); if (!token.session) return creation_id;
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        Json out = event(token, "resource_create", "after"); out.pointer("device", device); out.pointer("resource", resource);
        out.hex("resource_creation_id", creation_id); out.hex("reshade_initial_usage", initial_usage);
        out.text("coverage", "reshade_init_resource_path"); out.u32("dimension", static_cast<uint32_t>(desc.Dimension));
        out.hex("alignment", desc.Alignment); out.hex("width", desc.Width); out.u32("height", desc.Height);
        out.u32("depth_or_array_size", desc.DepthOrArraySize); out.u32("mip_levels", desc.MipLevels);
        out.u32("format", static_cast<uint32_t>(desc.Format)); out.u32("sample_count", desc.SampleDesc.Count);
        out.u32("sample_quality", desc.SampleDesc.Quality); out.u32("layout", static_cast<uint32_t>(desc.Layout));
        out.u32("flags", static_cast<uint32_t>(desc.Flags));
        if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) out.hex("gpu_va", resource->GetGPUVirtualAddress());
        out.boolean("gpu_bytes_read", false); send(token, out);
    } catch (...) { failure(token, "Resource creation observation exception"); }
    return creation_id;
}

void resource_destroyed(const void *device, const void *resource, uint64_t creation_id) noexcept {
    CallToken token {};
    try {
        Bridge &b = bridge(); bool matched = false;
        {
            std::lock_guard<std::mutex> lock(b.identity_mutex);
            auto it = b.resources.find(resource);
            if (it != b.resources.end() && (!creation_id || it->second == creation_id)) {
                creation_id = it->second; matched = true; b.resources.erase(it);
            }
        }
        token = new_call(); if (!token.session) return;
        Json out = event(token, "resource_destroy", "destruction_callback");
        out.pointer("device", device); out.pointer("resource", resource); out.hex("resource_creation_id", creation_id);
        out.boolean("identity_matched", matched); out.boolean("resource_dereferenced", false); send(token, out);
    } catch (...) { failure(token, "Resource destruction observation exception"); }
}

CallToken copy_as_before(const void *command_list, const void *native_command_list,
    D3D12_GPU_VIRTUAL_ADDRESS dest, D3D12_GPU_VIRTUAL_ADDRESS source,
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) noexcept {
    const CallToken token = new_call(); if (!token.session) return token;
    try {
        Json out = event(token, "copy_as", "before"); list_fields(out, command_list, native_command_list);
        out.hex("dest_as", dest); out.hex("source_as", source); out.u32("mode", static_cast<uint32_t>(mode)); send(token, out);
    } catch (...) { failure(token, "CopyAS observation exception"); }
    return token;
}

CallToken postbuild_before(const void *command_list, const void *native_command_list,
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc,
    UINT count, const D3D12_GPU_VIRTUAL_ADDRESS *source_addresses) noexcept {
    const CallToken token = new_call(); if (!token.session) return token;
    try {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC value {};
        const bool readable = cpu_read(desc, value);
        Json head = event(token, "postbuild_info", "before"); list_fields(head, command_list, native_command_list);
        head.u32("num_source_as", count); head.boolean("cpu_read_ok", readable);
        if (readable) { head.hex("dest_buffer_gpu_va", value.DestBuffer); head.u32("info_type", static_cast<uint32_t>(value.InfoType)); }
        send(token, head); bool complete = readable; uint32_t captured = 0;
        for (UINT i = 0; i < count; ++i) {
            D3D12_GPU_VIRTUAL_ADDRESS address = 0;
            const bool ok = cpu_index(source_addresses, i, address);
            Json out = event(token, "postbuild_source", "detail"); out.u32("source_index", i); out.boolean("cpu_read_ok", ok);
            if (ok) { out.hex("source_as", address); ++captured; } else complete = false;
            send(token, out); if (!ok) break;
        }
        Json end = event(token, "postbuild_snapshot", "before"); end.u32("sources_captured", captured); end.boolean("capture_complete", complete); send(token, end);
    } catch (...) { failure(token, "Postbuild info observation exception"); }
    return token;
}

} // namespace wwmi::native_trace
