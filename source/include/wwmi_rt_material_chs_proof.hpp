// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <guiddef.h>
#include <stdint.h>

namespace wwmi::rt_material_chs
{
	inline constexpr GUID private_data_guid = {
		0xcf3dc792, 0x77a3, 0x4fac, { 0x98, 0xd6, 0xd2, 0x76, 0xc5, 0xc1, 0x8f, 0x6b }
	};
	inline constexpr uint32_t version_v1 = 1, target_bytes = 12260, no_subobject = UINT32_MAX;
	inline constexpr char entry[] = "MaterialCHS";
	inline constexpr wchar_t native_entry[] = L"MaterialCHS";
	inline constexpr wchar_t native_export[] = L"CHS_65470ee2541674f9";
	inline constexpr wchar_t native_group[] = L"HitGroup_65470ee2541674f9";
	enum : uint32_t { local_collection = 1, whole_import, add_to_base };

	// Value on a successful native StateObject, set before SDK init_pipeline.
	// `code_address` is nonzero only on the source collection, and is usable
	// ONLY to compare with shader_desc.code in that collection's synchronous
	// init callback. Never dereference it or send it to the writer. The code
	// itself comes from the callback. All object handles are identities, not leases.
	struct Proof_v1
	{
		uint32_t size, version, edge, code_bytes;
		uint32_t dxil_index, group_index, local_root_index, association_index;
		uint32_t import_index, reserved;
		uint64_t generation, origin_generation, parent_generation;
		uint64_t origin_state, parent_state, local_root, code_address;
	};
	static_assert(sizeof(Proof_v1) == 96, "Material CHS private data ABI");

	inline bool valid(const Proof_v1 &p) noexcept
	{
		if (p.size != sizeof(p) || p.version != version_v1 || p.reserved || p.code_bytes != target_bytes ||
			!p.generation || !p.origin_generation || !p.origin_state || !p.local_root ||
			p.dxil_index == no_subobject || p.group_index == no_subobject ||
			p.local_root_index == no_subobject || p.association_index == no_subobject)
			return false;
		if (p.edge == local_collection)
			return p.generation == p.origin_generation && !p.parent_generation && !p.parent_state &&
				p.import_index == no_subobject && p.code_address != 0;
		return !p.code_address && p.parent_state && p.origin_generation <= p.parent_generation &&
			p.parent_generation < p.generation &&
			((p.edge == whole_import && p.import_index != no_subobject) ||
			 (p.edge == add_to_base && p.import_index == no_subobject));
	}
}
