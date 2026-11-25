// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

namespace wwmi::native_trace
{
	struct ResourceGenerations
	{
		uint64_t resource, counter;
	};

	// Existing native creation registry, even with no trace subscriber.
	// Identities only: does not dereference or retain either object.
	ResourceGenerations resource_generations(const void *resource, const void *counter = nullptr) noexcept;
}
