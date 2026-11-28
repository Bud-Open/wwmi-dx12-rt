// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "wwmi_upload_footprint.h"

namespace wwmi::upload_footprint {

inline thread_local const WWMI_D3D12UploadFootprint_v1 *current_notice = nullptr;

struct Notice {
	const WWMI_D3D12UploadFootprint_v1 *previous;

	explicit Notice(const WWMI_D3D12UploadFootprint_v1 *view) noexcept
		: previous(current_notice)
	{
		current_notice = view;
	}

	~Notice() noexcept
	{
		current_notice = previous;
	}

	Notice(const Notice &) = delete;
	Notice &operator=(const Notice &) = delete;
};

inline uint32_t query(
	uint64_t command_list,
	uint64_t src_resource,
	uint64_t dst_resource,
	uint32_t dst_subresource,
	WWMI_D3D12UploadFootprint_v1 *out,
	uint32_t bytes) noexcept
{
	const auto *fp = current_notice;
	if (!out || bytes != sizeof(*out) || !fp ||
		fp->command_list != command_list || fp->src_resource != src_resource ||
		fp->dst_resource != dst_resource || fp->dst_subresource != dst_subresource)
		return 0;
	*out = *fp;
	return 1;
}

} // namespace wwmi::upload_footprint
