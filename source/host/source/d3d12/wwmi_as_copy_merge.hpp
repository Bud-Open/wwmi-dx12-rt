// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#if RESHADE_ADDON >= 2
#include "wwmi_as_slots.hpp"

// Delegates to the exact-slot bitmap + 64-way sharded in-place node-reusing
// commit_copy in wwmi_as_slots.hpp.
namespace wwmi::rt_as::copy_merge
{
	inline void commit_copy(const Copy &copy)
	{
		wwmi::rt_as::commit_copy(copy);
	}
}
#endif
