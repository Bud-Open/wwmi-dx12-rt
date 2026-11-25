// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <guiddef.h>
#include <stdint.h>

namespace wwmi::rt_hit_group_graph
{
	inline constexpr GUID private_data_guid = {
		0x4486de74, 0x585c, 0x4c1c, { 0x94, 0x2f, 0x35, 0xf6, 0xca, 0xc7, 0x9d, 0x51 }
	};
	inline constexpr uint32_t version_v1 = 1;
	enum : uint32_t
	{
		complete = 1u << 0,
		triangles = 1u << 1,
		procedural = 1u << 2,
		known_bits = complete | triangles | procedural,
	};

	// Read from the actual live ID3D12StateObject using GetPrivateData, after
	// successful native creation (available during its SDK init_pipeline).
	// Missing data, a wrong size/version or unknown bits provide no proof.
	// COMPLETE without kind bits is known empty. Kind bits are positive
	// evidence; only COMPLETE allows ruling out a kind not present.
	// Reachable includes local groups, whole collection imports and AddTo base.
	struct Graph_v1
	{
		uint32_t size, version, local_bits, reachable_bits;
	};
	static_assert(sizeof(Graph_v1) == 16, "State-object private data ABI");
}
