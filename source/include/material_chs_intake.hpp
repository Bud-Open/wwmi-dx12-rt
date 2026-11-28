// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "wwmi_rt_material_chs_proof.hpp"
#include "rt_shader_support.hpp" // Existing synchronous CPU clone and CREATE_NEW writer.
#include <reshade.hpp>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace wwmi::rt_material_chs
{
namespace api = reshade::api;

enum : uint32_t { root_constants = 1, root_table, root_direct };
enum Failure : unsigned { writer_error, proof_missing, source_mismatch, source_copy, failure_count };
struct RootArg
{
	uint32_t kind, type, binding, reg, space, count, visibility, flags;
};
struct RootLayout
{
	uint64_t generation = 0;
	std::array<RootArg, 7> args {};
};
struct Job
{
	Proof_v1 proof {};
	uint64_t state = 0, device = 0, declared_global_root = 0, qpc = 0;
	uint32_t thread = 0;
	RootLayout root {};
	std::vector<uint8_t> code;
};

// Same producer/consumer boundary as the accepted native shader writer:
// synchronous owned CPU copy, one wake-driven worker, no I/O with the queue
// mutex held, drain at AddonUninit. The old observer and its quota stay off.
class Writer
{
	std::atomic<bool> enabled_ { false };
	std::mutex mutex_;
	std::deque<Job> pending_;
	HANDLE worker_ = nullptr, wake_ = nullptr;
	HMODULE owner_ = nullptr;
	bool stopping_ = false;
	std::wstring directory_;
	std::string session_;
	uint32_t pid_ = 0;
	uint64_t frequency_ = 0;
	std::atomic<unsigned> queued_source_ { 0 }, queued_edge_ { 0 }, source_receipts_ { 0 }, edge_receipts_ { 0 };
	std::array<std::atomic<unsigned>, failure_count> failures_ {};

	void write(const Job &job)
	{
		const auto &p = job.proof;
		const bool source = p.edge == local_collection;
		const auto stem = directory_ + L"\\wwmi-material-chs-p" + std::to_wstring(pid_) + L"-state" + std::to_wstring(p.generation);
		if (source && !rt_write_new(stem + L".dxil", job.code)) { fail(); return; }
		std::ostringstream out;
		out << "version=1 session=" << session_ << " process=" << pid_ << " thread=" << job.thread
			<< " qpc=" << job.qpc << " frequency=" << frequency_ << '\n'
			<< "native_success=1 edge=" << (source ? "local_collection" : p.edge == whole_import ? "whole_import" : "add_to_base")
			<< " state_generation=" << p.generation << " origin_generation=" << p.origin_generation
			<< " parent_generation=" << p.parent_generation << " origin_code_bytes=" << p.code_bytes
			<< " owned_code_written=" << source << '\n'
			<< "device=0x" << std::hex << job.device << " state=0x" << job.state << " origin_state=0x" << p.origin_state
			<< " parent_state=0x" << p.parent_state << " local_root=0x" << p.local_root
			<< " declared_global_root=0x" << job.declared_global_root << std::dec << '\n'
			<< "entry=MaterialCHS export=CHS_65470ee2541674f9 hit_group=HitGroup_65470ee2541674f9 group_type=triangles\n"
			<< "dxil_subobject=" << p.dxil_index << " group_subobject=" << p.group_index
			<< " local_root_subobject=" << p.local_root_index << " association_subobject=" << p.association_index
			<< " import_subobject=" << p.import_index << " active_hit_proven=0\n";
		if (source)
		{
			out << "local_layout_observed=" << bool(job.root.generation) << " local_layout_generation=" << job.root.generation << '\n';
			if (job.root.generation)
				for (unsigned i = 0; i < job.root.args.size(); ++i)
				{
					const auto &a = job.root.args[i];
					out << "root_param=" << i << " kind=" << (a.kind == root_constants ? "constants" : a.kind == root_table ? "table" : "direct")
						<< " sdk_descriptor_type=" << a.type
						<< " binding=" << a.binding << " register=" << a.reg << " space=" << a.space
						<< " count=" << a.count << " visibility=" << a.visibility << " flags=" << a.flags << '\n';
				}
		}
		const auto text = out.str();
		const std::vector<uint8_t> receipt(text.begin(), text.end());
		if (!rt_write_new(stem + L".receipt.txt", receipt)) { fail(); return; }
		(source ? source_receipts_ : edge_receipts_).fetch_add(1, std::memory_order_relaxed);
	}
	static DWORD WINAPI run(void *context) noexcept
	{
		auto &self = *static_cast<Writer *>(context);
		for (;;)
		{
			Job job;
			{
				std::unique_lock lock(self.mutex_);
				if (self.pending_.empty())
				{
					if (self.stopping_) return 0;
					lock.unlock();
					if (WaitForSingleObject(self.wake_, INFINITE) != WAIT_OBJECT_0) { self.fail(); return 1; }
					continue;
				}
				job = std::move(self.pending_.front());
				self.pending_.pop_front();
			}
			try { self.write(job); } catch (...) { self.fail(); }
		}
	}

public:
	bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
	void fail(Failure why = writer_error) noexcept { failures_[why].fetch_add(1, std::memory_order_relaxed); }
	void start() noexcept
	{
		if (worker_) return;
		wchar_t directory[1024] {};
		char session[128] {};
		const DWORD dn = GetEnvironmentVariableW(L"WWMI_RT_CAPTURE_DIR", directory, 1024);
		const DWORD sn = GetEnvironmentVariableA("WWMI_RT_CAPTURE_SESSION", session, 128);
		if (!dn || dn >= 1024 || !sn || sn >= 128) return;
		LARGE_INTEGER hz {};
		if (!QueryPerformanceFrequency(&hz)) return;
		try { directory_.assign(directory, dn); session_.assign(session, sn); }
		catch (...) { fail(); return; }
		pid_ = GetCurrentProcessId(); frequency_ = hz.QuadPart;
		stopping_ = false;
		// Keep the same module mapped if an I/O operation cannot finish within
		// the accepted observer's bounded shutdown wait. The worker never calls
		// the host or holds a device/resource lease.
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&run), &owner_))
		{
			wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (wake_) worker_ = CreateThread(nullptr, 0, &run, this, 0, nullptr);
			if (worker_) { enabled_.store(true, std::memory_order_release); return; }
			if (wake_) CloseHandle(std::exchange(wake_, nullptr));
			FreeLibrary(std::exchange(owner_, nullptr));
		}
		reshade::log::message(reshade::log::level::warning, "RT-MATERIAL-CHS writer_start_failed");
	}
	void push(Job &&job)
	{
		std::lock_guard lock(mutex_);
		if (!enabled()) return;
		const bool source = job.proof.edge == local_collection;
		pending_.push_back(std::move(job));
		(source ? queued_source_ : queued_edge_).fetch_add(1, std::memory_order_relaxed);
		SetEvent(wake_);
	}
	void stop() noexcept
	{
		if (!worker_) return;
		{
			std::lock_guard lock(mutex_);
			enabled_.store(false, std::memory_order_release);
			stopping_ = true;
			SetEvent(wake_);
		}
		DWORD exit = 1;
		const bool stopped = WaitForSingleObject(worker_, 10000) == WAIT_OBJECT_0;
		const bool drained = stopped && GetExitCodeThread(worker_, &exit) && exit == 0;
		if (stopped)
		{
			CloseHandle(std::exchange(worker_, nullptr));
			CloseHandle(std::exchange(wake_, nullptr));
			FreeLibrary(std::exchange(owner_, nullptr));
		}
		char summary[320];
		std::snprintf(summary, sizeof(summary), "RT-MATERIAL-CHS drained=%u source_jobs=%u edge_jobs=%u source_receipts=%u edge_receipts=%u writer_error=%u native_proof_missing=%u source_mismatch=%u source_copy_failed=%u",
			unsigned(drained), queued_source_.load(), queued_edge_.load(), source_receipts_.load(), edge_receipts_.load(),
			failures_[writer_error].load(), failures_[proof_missing].load(), failures_[source_mismatch].load(), failures_[source_copy].load());
		reshade::log::message(drained ? reshade::log::level::info : reshade::log::level::warning, summary);
	}
};

class Intake
{
	std::unordered_map<uint64_t, RootLayout> layouts_;
	uint64_t layout_generation_ = 0;
	std::vector<unsigned char> exact_pin_;
	struct Program
	{
		uint64_t generation, origin, root;
		RootLayout layout;
		std::optional<std::array<std::byte, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES>> identifier;
	};
	std::unordered_map<uint64_t, Program> programs_;

	static bool exact_root(const RootLayout &root) noexcept
	{
		const auto &a = root.args;
		constexpr unsigned registers[]{0, 1, 0, 0, 0, 0, 1};
		constexpr unsigned spaces[]{2, 2, 2, 0, 0, 0, 0};
		constexpr unsigned counts[]{1, 1, 4, 4, 2, 1, 1};
		for (unsigned i = 0; i < a.size(); ++i)
			if (a[i].binding || a[i].reg != registers[i] || a[i].space != spaces[i] ||
				a[i].count != counts[i] || a[i].visibility != uint32_t(api::shader_stage::all)) return false;
		return root.generation != 0; // kind/type already checked at layout creation
	}
	void retain_program(ID3D12StateObject *object, const Proof_v1 &proof, const api::shader_desc *shader)
	{
		if (exact_pin_.empty()) return;
		RootLayout root;
		if (proof.edge == local_collection)
		{
			if (!shader || shader->code_size != exact_pin_.size() ||
				std::memcmp(shader->code, exact_pin_.data(), exact_pin_.size())) return;
			const auto it = layouts_.find(proof.local_root);
			if (it == layouts_.end() || !exact_root(it->second)) return;
			root = it->second;
		}
		else
		{
			const auto it = programs_.find(proof.parent_state);
			if (it == programs_.end() || it->second.generation != proof.parent_generation ||
				it->second.origin != proof.origin_generation || it->second.root != proof.local_root) return;
			root = it->second.layout;
		}
		Program result { proof.generation, proof.origin_generation, proof.local_root, root, {} };
		Microsoft::WRL::ComPtr<ID3D12StateObjectProperties> properties;
		if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&properties))))
			if (const void *p = properties->GetShaderIdentifier(native_group))
			{
				result.identifier.emplace();
				std::memcpy(result.identifier->data(), p, result.identifier->size());
			}
		programs_.insert_or_assign(reinterpret_cast<uint64_t>(object), std::move(result));
	}

	static bool root_arg(const api::pipeline_layout_param &param, RootArg &out) noexcept
	{
		using Type = api::pipeline_layout_param_type;
		const api::descriptor_range *range = nullptr;
		uint32_t flags = 0;
		switch (param.type)
		{
		case Type::push_constants:
		{
			const auto &c = param.push_constants;
			out = { root_constants, UINT32_MAX, c.binding, c.dx_register_index, c.dx_register_space, c.count, uint32_t(c.visibility), 0 };
			return true;
		}
		case Type::push_descriptors: range = &param.push_descriptors; break;
		case Type::descriptor_table:
			if (param.descriptor_table.count != 1) return false;
			range = param.descriptor_table.ranges;
			break;
		case Type::push_descriptors_with_ranges_and_flags:
		case Type::descriptor_table_with_flags:
			if (param.descriptor_table_with_flags.count != 1 || !param.descriptor_table_with_flags.ranges ||
				param.descriptor_table_with_flags.ranges[0].static_samplers) return false;
			flags = uint32_t(param.descriptor_table_with_flags.ranges[0].flags);
			range = param.descriptor_table_with_flags.ranges;
			break;
		default: return false;
		}
		if (!range) return false;
		const bool table = param.type == Type::descriptor_table || param.type == Type::descriptor_table_with_flags;
		out = { table ? root_table : root_direct, uint32_t(range->type), range->binding, range->dx_register_index,
			range->dx_register_space, range->count, uint32_t(range->visibility), flags };
		return true;
	}

public:
	void pin(std::vector<unsigned char> bytes)
	{
		if (bytes.size() == target_bytes) exact_pin_ = std::move(bytes);
	}
	void destroy_pipeline(api::pipeline state) noexcept { programs_.erase(state.handle); }
	bool has_hit_program(api::pipeline state) const noexcept
	{
		const auto p = programs_.find(state.handle);
		return p != programs_.end() && bool(p->second.identifier);
	}
	struct HitArguments
	{
		uint64_t index_va, position_va;
		uint32_t system[4];
		uint64_t local_srv_table, local_sampler_table, local_vf_cb, material_cb;
	};
	// Only decode bytes from a caller's actual selected HitGroupTable record.
	// This gives native argument values, not descriptor/resource leases.
	std::optional<HitArguments> match_hit(api::pipeline state, std::span<const std::byte> record) const noexcept
	{
		static_assert(sizeof(HitArguments) == 64);
		const auto p = programs_.find(state.handle);
		constexpr auto id_size = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
		if (p == programs_.end() || !p->second.identifier || record.size() < id_size + sizeof(HitArguments) ||
			std::memcmp(record.data(), p->second.identifier->data(), id_size)) return {};
		HitArguments args {};
		std::memcpy(&args, record.data() + id_size, sizeof(args));
		return args;
	}
	// The existing Runtime lock owns this small candidate-layout map. Hold only
	// the observed seven-argument shape, not full layouts for unrelated pipelines.
	void layout(Writer &writer, api::pipeline_layout layout, unsigned count, const api::pipeline_layout_param *params) noexcept
	{
		if ((!writer.enabled() && exact_pin_.empty()) || !layout.handle || count != 7 || !params) return;
		RootLayout value;
		for (unsigned i = 0; i < value.args.size(); ++i)
			if (!root_arg(params[i], value.args[i])) return;
		using D = api::descriptor_type;
		const auto &a = value.args;
		if (a[0].kind != root_direct || a[0].type != uint32_t(D::buffer_shader_resource_view) ||
			a[1].kind != root_direct || a[1].type != uint32_t(D::buffer_shader_resource_view) || a[2].kind != root_constants ||
			a[3].kind != root_table || a[3].type != uint32_t(D::shader_resource_view) || a[4].kind != root_table || a[4].type != uint32_t(D::sampler) ||
			a[5].kind != root_direct || a[5].type != uint32_t(D::constant_buffer) || a[6].kind != root_direct || a[6].type != uint32_t(D::constant_buffer)) return;
		value.generation = ++layout_generation_;
		try { layouts_.insert_or_assign(layout.handle, value); } catch (...) { writer.fail(); }
	}
	void destroy_layout(api::pipeline_layout layout) noexcept { layouts_.erase(layout.handle); }
	bool active(const Writer &writer) const noexcept { return writer.enabled() || !exact_pin_.empty(); }
	void pipeline(Writer &writer, uint64_t device, api::pipeline state, api::pipeline_layout declared_global_root,
		unsigned count, const api::pipeline_subobject *subobjects) noexcept
	{
		if ((!writer.enabled() && exact_pin_.empty()) || !state.handle || !subobjects) return;
		programs_.erase(state.handle);
		const api::shader_desc *shader = nullptr;
		bool libraries = false, multiple = false;
		for (unsigned i = 0; i < count; ++i)
		{
			const auto &sub = subobjects[i];
			libraries |= sub.type == api::pipeline_subobject_type::libraries && sub.count != 0;
			if (sub.type != api::pipeline_subobject_type::closest_hit_shader || !sub.data) continue;
			const auto *values = static_cast<const api::shader_desc *>(sub.data);
			for (unsigned j = 0; j < sub.count; ++j)
				if (const auto &s = values[j]; s.code && s.code_size == target_bytes && s.entry_point && !std::strcmp(s.entry_point, entry))
				{
					multiple |= shader != nullptr;
					shader = &s;
				}
		}
		if (!shader && !libraries) return;
		Job job;
		auto &proof = job.proof;
		UINT size = sizeof(proof);
		auto *object = reinterpret_cast<ID3D12StateObject *>(state.handle);
		if (FAILED(object->GetPrivateData(private_data_guid, &size, &proof)) || size != sizeof(proof) || !valid(proof))
		{
			if (shader && writer.enabled()) writer.fail(proof_missing);
			return;
		}
		try
		{
			LARGE_INTEGER qpc {};
			QueryPerformanceCounter(&qpc);
			job.state = state.handle; job.device = device; job.declared_global_root = declared_global_root.handle;
			job.thread = GetCurrentThreadId(); job.qpc = qpc.QuadPart;
			if (proof.edge == local_collection)
			{
				if (!shader || multiple || proof.origin_state != state.handle ||
					proof.code_address != reinterpret_cast<uint64_t>(shader->code)) {
					if (writer.enabled()) writer.fail(source_mismatch);
					return;
				}
				if (const auto it = layouts_.find(proof.local_root); it != layouts_.end()) job.root = it->second;
			}
			retain_program(object,proof,shader);
			if (!writer.enabled()) return;
			if (proof.edge == local_collection && !rt_cpu_copy(shader->code, proof.code_bytes, job.code)) {
				writer.fail(source_copy); return;
			}
			proof.code_address = 0; // Never carry the borrowed input into the queue.
			writer.push(std::move(job));
		}
		catch (...) { writer.fail(); }
	}
};
}
