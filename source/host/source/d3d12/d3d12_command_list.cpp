/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include "d3d12_device.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_impl_type_convert.hpp"
#include "dll_log.hpp"
#include "com_utils.hpp"
#include "addon_manager.hpp"
#include "wwmi_native_trace.hpp"
#include "wwmi_rt_dispatch_scope.hpp"
#include "native_buffer_lease.hpp"
namespace wwmi::rt_forward {void buffer(reshade::api::command_list *,ID3D12GraphicsCommandList *,uint32_t,uint32_t,ID3D12Resource *,ID3D12Resource *,uint64_t,uint64_t,uint64_t,const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *) noexcept;}
#include "wwmi_binding_trace_d3d12.hpp"
#include "wwmi_upload_footprint_bridge.hpp"

extern "C" uint32_t __cdecl WWMI_CurrentUploadFootprint_v1(
	uint64_t command_list, uint64_t src_resource, uint64_t dst_resource,
	uint32_t dst_subresource, WWMI_D3D12UploadFootprint_v1 *out, uint32_t bytes)
{
	return wwmi::upload_footprint::query(
		command_list, src_resource, dst_resource, dst_subresource, out, bytes);
}

using reshade::d3d12::to_handle;
namespace wcs = wwmi::compute_state;
using hsin_rt::RootKind;
using hsin_rt::PipelineKind;

// All native-buffer facts and ABI consumers use the ReShade API subobject,
// not the distinct COM proxy address in this multiple-inheritance wrapper.
static uint64_t native_buffer_recording_id(D3D12GraphicsCommandList *list) noexcept
{
	return reinterpret_cast<uint64_t>(static_cast<reshade::api::command_list *>(list));
}

D3D12GraphicsCommandList::D3D12GraphicsCommandList(D3D12Device *device, ID3D12GraphicsCommandList *original, ID3D12PipelineState *initial_state) :
	command_list_impl(device, original)
{
	assert(_orig != nullptr && _device != nullptr);
	set_private_data(reinterpret_cast<const uint8_t *>(&__uuidof(wcs::State)), reinterpret_cast<uint64_t>(&_wwmi_compute));
	// Seed the native initial state before add-ons can record real setters.
	wcs::reset(_wwmi_compute, reinterpret_cast<uint64_t>(initial_state));
	wwmi::native_buffer::reset(native_buffer_recording_id(this));
	wwmi::native_trace::command_list_created(this, _orig);
	wwmi::binding_trace::created(_orig, this, 1);

#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::init_command_list>(this);
#endif
}
D3D12GraphicsCommandList::~D3D12GraphicsCommandList()
{
	wwmi::native_buffer::destroy(native_buffer_recording_id(this));
	_wwmi_compute.snapshot.invalidate();
	set_private_data(reinterpret_cast<const uint8_t *>(&__uuidof(wcs::State)), 0);
	wwmi::binding_trace::destroyed(_orig, this, 1);
	wwmi::native_trace::command_list_destroyed(this, _orig);
#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::destroy_command_list>(this);
#endif
}

bool D3D12GraphicsCommandList::check_and_upgrade_interface(REFIID riid)
{
	if (riid == __uuidof(this) ||
		riid == __uuidof(IUnknown) ||
		riid == __uuidof(ID3D12Object) ||
		riid == __uuidof(ID3D12DeviceChild) ||
		riid == __uuidof(ID3D12CommandList))
		return true;

	static constexpr IID iid_lookup[] = {
		__uuidof(ID3D12GraphicsCommandList),
		__uuidof(ID3D12GraphicsCommandList1),
		__uuidof(ID3D12GraphicsCommandList2),
		__uuidof(ID3D12GraphicsCommandList3),
		__uuidof(ID3D12GraphicsCommandList4),
		__uuidof(ID3D12GraphicsCommandList5),
		__uuidof(ID3D12GraphicsCommandList6),
		__uuidof(ID3D12GraphicsCommandList7),
		__uuidof(ID3D12GraphicsCommandList8),
		__uuidof(ID3D12GraphicsCommandList9),
		__uuidof(ID3D12GraphicsCommandList10),
	};

	for (unsigned short version = 0; version < std::size(iid_lookup); ++version)
	{
		if (riid != iid_lookup[version])
			continue;

		if (version > _interface_version)
		{
			IUnknown *new_interface = nullptr;
			if (FAILED(_orig->QueryInterface(riid, reinterpret_cast<void **>(&new_interface))))
				return false;
#if 0
			reshade::log::message(reshade::log::level::debug, "Upgrading ID3D12GraphicsCommandList%hu object %p to ID3D12GraphicsCommandList%hu.", _interface_version, this, version);
#endif
			if (static_cast<IUnknown *>(_orig) != new_interface)
			{
				wwmi::binding_trace::invalidated(_orig, this, 1, 1090, WWMI_BIND_REASON_EXTERNAL_NATIVE);
				wwmi::binding_trace::invalidated(new_interface, this, 1, 1091, WWMI_BIND_REASON_UNOBSERVED_LIFETIME);
			}
			_orig->Release();
			_orig = static_cast<ID3D12GraphicsCommandList *>(new_interface);
			_interface_version = version;
		}

		return true;
	}

	return false;
}

HRESULT STDMETHODCALLTYPE D3D12GraphicsCommandList::QueryInterface(REFIID riid, void **ppvObj)
{
	if (ppvObj == nullptr)
		return E_POINTER;

	if (check_and_upgrade_interface(riid))
	{
		AddRef();
		*ppvObj = this;
		return S_OK;
	}

	// Interface ID to query the original object from a proxy object
	if (riid == IID_UnwrappedObject)
	{
		_orig->AddRef();
		*ppvObj = _orig;
		return S_OK;
	}

	return _orig->QueryInterface(riid, ppvObj);
}
ULONG   STDMETHODCALLTYPE D3D12GraphicsCommandList::AddRef()
{
	_orig->AddRef();
	return InterlockedIncrement(&_ref);
}
ULONG   STDMETHODCALLTYPE D3D12GraphicsCommandList::Release()
{
	const ULONG ref = InterlockedDecrement(&_ref);
	if (ref != 0)
	{
		_orig->Release();
		return ref;
	}

	const auto orig = _orig;
	const auto interface_version = _interface_version;
#if 0
	reshade::log::message(reshade::log::level::debug, "Destroying ID3D12GraphicsCommandList%hu object %p (%p).", interface_version, this, orig);
#endif
	delete this;

	const ULONG ref_orig = orig->Release();
	if (ref_orig != 0) // Verify internal reference count
		reshade::log::message(reshade::log::level::warning, "Reference count for ID3D12GraphicsCommandList%hu object %p (%p) is inconsistent (%lu).", interface_version, this, orig, ref_orig);
	return 0;
}

HRESULT STDMETHODCALLTYPE D3D12GraphicsCommandList::GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData)
{
	return _orig->GetPrivateData(guid, pDataSize, pData);
}
HRESULT STDMETHODCALLTYPE D3D12GraphicsCommandList::SetPrivateData(REFGUID guid, UINT DataSize, const void *pData)
{
	return _orig->SetPrivateData(guid, DataSize, pData);
}
HRESULT STDMETHODCALLTYPE D3D12GraphicsCommandList::SetPrivateDataInterface(REFGUID guid, const IUnknown *pData)
{
	return _orig->SetPrivateDataInterface(guid, pData);
}
HRESULT STDMETHODCALLTYPE D3D12GraphicsCommandList::SetName(LPCWSTR Name)
{
	return _orig->SetName(Name);
}

HRESULT STDMETHODCALLTYPE D3D12GraphicsCommandList::GetDevice(REFIID riid, void **ppvDevice)
{
	return static_cast<D3D12Device *>(_device)->QueryInterface(riid, ppvDevice);
}

D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE D3D12GraphicsCommandList::GetType()
{
	return _orig->GetType();
}

HRESULT STDMETHODCALLTYPE D3D12GraphicsCommandList::Close()
{
#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::close_command_list>(this);
#endif

	const HRESULT hr = wwmi::binding_trace::close(_orig, 1, 1001);
	wwmi::native_trace::command_list_close_after(this, _orig, hr);
	return hr;
}
HRESULT STDMETHODCALLTYPE D3D12GraphicsCommandList::Reset(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialState)
{
#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::reset_command_list>(this);
#endif

	_current_root_signature[0] = nullptr;
	_current_root_signature[1] = nullptr;
	_current_descriptor_heaps[0] = nullptr;
	_current_descriptor_heaps[1] = nullptr;
#if RESHADE_ADDON >= 2
	_previous_descriptor_heaps[0] = nullptr;
	_previous_descriptor_heaps[1] = nullptr;
#endif

	const HRESULT hr = wwmi::binding_trace::reset(_orig, 1, 1002, pAllocator, pInitialState);
	if (SUCCEEDED(hr)) {wcs::reset(_wwmi_compute, reinterpret_cast<uint64_t>(pInitialState));wwmi::native_buffer::reset(native_buffer_recording_id(this));}
	else wcs::unobserved(_wwmi_compute);
	wwmi::native_trace::command_list_reset_after(this, _orig, pAllocator, pInitialState, hr);
#if RESHADE_ADDON >= 2
	if (SUCCEEDED(hr))
	{
		// Only invoke event if there actually is an initial state to bind, otherwise expect things were already handled by the 'reset_command_list' event above
		if (pInitialState != nullptr)
			reshade::invoke_addon_event<reshade::addon_event::bind_pipeline>(this, reshade::api::pipeline_stage::all, to_handle(pInitialState));
	}
#endif

	return hr;
}

void STDMETHODCALLTYPE D3D12GraphicsCommandList::ClearState(ID3D12PipelineState *pPipelineState)
{
	wwmi::binding_trace::clear(_orig, 1, 1003, pPipelineState);
	wcs::reset(_wwmi_compute, reinterpret_cast<uint64_t>(pPipelineState));

	_current_root_signature[0] = nullptr;
	_current_root_signature[1] = nullptr;
	_current_descriptor_heaps[0] = nullptr;
	_current_descriptor_heaps[1] = nullptr;
#if RESHADE_ADDON >= 2
	_previous_descriptor_heaps[0] = nullptr;
	_previous_descriptor_heaps[1] = nullptr;

	reshade::invoke_addon_event<reshade::addon_event::bind_pipeline>(this, reshade::api::pipeline_stage::all, to_handle(pPipelineState));

	// When 'ClearState' is called, all currently bound resources are unbound.
	// The primitive topology is set to D3D_PRIMITIVE_TOPOLOGY_UNDEFINED. Viewports, scissor rectangles, stencil reference value, and the blend factor are set to empty values (all zeros).
	const reshade::api::dynamic_state states[4] = { reshade::api::dynamic_state::primitive_topology, reshade::api::dynamic_state::blend_constant, reshade::api::dynamic_state::front_stencil_reference_value, reshade::api::dynamic_state::back_stencil_reference_value };
	const uint32_t values[4] = { static_cast<uint32_t>(reshade::api::primitive_topology::undefined), 0, 0, 0 };
	reshade::invoke_addon_event<reshade::addon_event::bind_pipeline_states>(this, static_cast<uint32_t>(std::size(states)), states, values);

	constexpr size_t max_null_objects = D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT * 2;
	void *const null_objects[max_null_objects] = {};

	reshade::invoke_addon_event<reshade::addon_event::bind_descriptor_tables>(this, reshade::api::shader_stage::all, reshade::api::pipeline_layout {}, 0, 0, nullptr);

	reshade::invoke_addon_event<reshade::addon_event::bind_index_buffer>(this, reshade::api::resource {}, 0, 0);
	reshade::invoke_addon_event<reshade::addon_event::bind_vertex_buffers>(this, 0, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, reinterpret_cast<const reshade::api::resource *>(null_objects), reinterpret_cast<const uint64_t *>(null_objects), reinterpret_cast<const uint32_t *>(null_objects));

	reshade::invoke_addon_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(this, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT, reinterpret_cast<const reshade::api::resource_view *>(null_objects), reshade::api::resource_view {});

	reshade::invoke_addon_event<reshade::addon_event::bind_viewports>(this, 0, 0, nullptr);
	reshade::invoke_addon_event<reshade::addon_event::bind_scissor_rects>(this, 0, 0, nullptr);
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)
{
#if RESHADE_ADDON
	if (reshade::invoke_addon_event<reshade::addon_event::draw>(this, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation))
		return;
#endif
	_orig->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
#if RESHADE_ADDON
	if (reshade::invoke_addon_event<reshade::addon_event::draw_indexed>(this, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation))
		return;
#endif
	wwmi::binding_trace::draw_indexed(_orig, 1, 1004, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ)
{
#if RESHADE_ADDON
	const bool cancelled = reshade::invoke_addon_event<reshade::addon_event::dispatch>(this, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
	wwmi::rt_forward::DispatchScope receipt(this, _orig, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
	if (receipt)
	{
		const auto &state = _wwmi_compute;
		const auto epoch = wcs::current_epoch();
		const bool known = (epoch & 1) && state.epoch == epoch && state.recording_known && !state.inside &&
			state.snapshot.ready() && state.snapshot.pipeline_kind() == PipelineKind::ordinary;
		if (cancelled)
			receipt.cancelled(known, state.snapshot.pipeline_value(), state.snapshot.root_value());
		else
			receipt.before(known, state.snapshot.pipeline_value(), state.snapshot.root_value());
	}
	if (cancelled)
		return;
#endif
	_orig->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
#if RESHADE_ADDON
	if (receipt)
		receipt.after();
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::CopyBufferRegion(ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes)
{
#if RESHADE_ADDON >= 2
	assert(pDstBuffer != nullptr && pSrcBuffer != nullptr);

	if (reshade::invoke_addon_event<reshade::addon_event::copy_buffer_region>(this, to_handle(pSrcBuffer), SrcOffset, to_handle(pDstBuffer), DstOffset, NumBytes))
		return;
#endif
	wwmi::rt_forward::buffer(this,_orig,WWMI_RT_BUFFER_COPY,WWMI_RT_BUFFER_BEFORE_NATIVE,pSrcBuffer,pDstBuffer,SrcOffset,DstOffset,NumBytes,nullptr);
	_orig->CopyBufferRegion(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes);
	wwmi::rt_forward::buffer(this,_orig,WWMI_RT_BUFFER_COPY,WWMI_RT_BUFFER_NATIVE_RECORDED,pSrcBuffer,pDstBuffer,SrcOffset,DstOffset,NumBytes,nullptr);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION *pSrc, const D3D12_BOX *pSrcBox)
{
#if RESHADE_ADDON >= 2
	assert(pDst != nullptr && pSrc != nullptr && pDst->pResource != nullptr && pSrc->pResource != nullptr);

	if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
		reshade::has_addon_event<reshade::addon_event::copy_texture_region>())
	{
		const bool use_dst_box = DstX != 0 || DstY != 0 || DstZ != 0;
		reshade::api::subresource_box dst_box;

		if (use_dst_box)
		{
			dst_box.left = DstX;
			dst_box.top = DstY;
			dst_box.front = DstZ;

			if (pSrcBox != nullptr)
			{
				dst_box.right = dst_box.left + pSrcBox->right - pSrcBox->left;
				dst_box.bottom = dst_box.top + pSrcBox->bottom - pSrcBox->top;
				dst_box.back = dst_box.front + pSrcBox->back - pSrcBox->front;
			}
			else
			{
				const D3D12_RESOURCE_DESC desc = pSrc->pResource->GetDesc();

				dst_box.right = dst_box.left + static_cast<UINT>(desc.Width);
				dst_box.bottom = dst_box.top + desc.Height;
				dst_box.back = dst_box.front + (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? desc.DepthOrArraySize : 1u);
			}
		}

		if (reshade::invoke_addon_event<reshade::addon_event::copy_texture_region>(
				this,
				to_handle(pSrc->pResource),
				pSrc->SubresourceIndex,
				reinterpret_cast<const reshade::api::subresource_box *>(pSrcBox),
				to_handle(pDst->pResource),
				pDst->SubresourceIndex,
				use_dst_box ? &dst_box : nullptr,
				reshade::api::filter_mode::min_mag_mip_point))
			return;
	}
	else
	if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT && pDst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX &&
		reshade::has_addon_event<reshade::addon_event::copy_buffer_to_texture>())
	{
		reshade::api::subresource_box dst_box;
		dst_box.left = DstX;
		dst_box.top = DstY;
		dst_box.front = DstZ;

		if (pSrcBox != nullptr)
		{
			dst_box.right = dst_box.left + pSrcBox->right - pSrcBox->left;
			dst_box.bottom = dst_box.top + pSrcBox->bottom - pSrcBox->top;
			dst_box.back = dst_box.front + pSrcBox->back - pSrcBox->front;
		}
		else
		{
			dst_box.right = dst_box.left + pSrc->PlacedFootprint.Footprint.Width;
			dst_box.bottom = dst_box.top + pSrc->PlacedFootprint.Footprint.Height;
			dst_box.back = dst_box.front + pSrc->PlacedFootprint.Footprint.Depth;
		}

		const WWMI_D3D12UploadFootprint_v1 upload_fp {
			sizeof(WWMI_D3D12UploadFootprint_v1),
			1u,
			native_buffer_recording_id(this),
			to_handle(pSrc->pResource).handle,
			to_handle(pDst->pResource).handle,
			pDst->SubresourceIndex,
			pSrc->PlacedFootprint.Footprint.RowPitch,
			pSrc->PlacedFootprint.Footprint.Width,
			pSrc->PlacedFootprint.Footprint.Height,
			pSrc->PlacedFootprint.Footprint.Depth,
			pSrcBox ? pSrcBox->left : 0u,
			pSrcBox ? pSrcBox->top : 0u,
			pSrcBox ? pSrcBox->front : 0u,
			pSrcBox ? 1u : 0u,
			0u
		};
		const wwmi::upload_footprint::Notice upload_fp_notice(&upload_fp);

		if (reshade::invoke_addon_event<reshade::addon_event::copy_buffer_to_texture>(
				this,
				to_handle(pSrc->pResource),
				pSrc->PlacedFootprint.Offset,
				pSrc->PlacedFootprint.Footprint.Width,
				pSrc->PlacedFootprint.Footprint.Height,
				to_handle(pDst->pResource),
				pDst->SubresourceIndex,
				&dst_box))
			return;
	}
	else
	if (pSrc->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX && pDst->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT &&
		reshade::has_addon_event<reshade::addon_event::copy_texture_to_buffer>())
	{
		assert(DstX == 0 && DstY == 0 && DstZ == 0);

		if (reshade::invoke_addon_event<reshade::addon_event::copy_texture_to_buffer>(
				this,
				to_handle(pSrc->pResource),
				pSrc->SubresourceIndex, reinterpret_cast<const reshade::api::subresource_box *>(pSrcBox),
				to_handle(pDst->pResource),
				pDst->PlacedFootprint.Offset,
				pDst->PlacedFootprint.Footprint.Width,
				pDst->PlacedFootprint.Footprint.Height))
			return;
	}
#endif

	_orig->CopyTextureRegion(pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::CopyResource(ID3D12Resource *pDstResource, ID3D12Resource *pSrcResource)
{
#if RESHADE_ADDON >= 2
	assert(pDstResource != nullptr && pSrcResource != nullptr);

	if (reshade::invoke_addon_event<reshade::addon_event::copy_resource>(this, to_handle(pSrcResource), to_handle(pDstResource)))
		return;
#endif
	const auto wwmi_copy_desc=pDstResource->GetDesc();
	const auto wwmi_copy_bytes=wwmi_copy_desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER?wwmi_copy_desc.Width:0;
	if(wwmi_copy_bytes)wwmi::rt_forward::buffer(this,_orig,WWMI_RT_BUFFER_COPY,WWMI_RT_BUFFER_BEFORE_NATIVE,pSrcResource,pDstResource,0,0,wwmi_copy_bytes,nullptr);
	_orig->CopyResource(pDstResource, pSrcResource);
	if(wwmi_copy_bytes)wwmi::rt_forward::buffer(this,_orig,WWMI_RT_BUFFER_COPY,WWMI_RT_BUFFER_NATIVE_RECORDED,pSrcResource,pDstResource,0,0,wwmi_copy_bytes,nullptr);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::CopyTiles(ID3D12Resource *pTiledResource, const D3D12_TILED_RESOURCE_COORDINATE *pTileRegionStartCoordinate, const D3D12_TILE_REGION_SIZE *pTileRegionSize, ID3D12Resource *pBuffer, UINT64 BufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS Flags)
{
	_orig->CopyTiles(pTiledResource, pTileRegionStartCoordinate, pTileRegionSize, pBuffer, BufferStartOffsetInBytes, Flags);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::ResolveSubresource(ID3D12Resource *pDstResource, UINT DstSubresource, ID3D12Resource *pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format)
{
#if RESHADE_ADDON >= 2
	assert(pDstResource != nullptr && pSrcResource != nullptr);

	if (reshade::invoke_addon_event<reshade::addon_event::resolve_texture_region>(this, to_handle(pSrcResource), SrcSubresource, nullptr, to_handle(pDstResource), DstSubresource, 0, 0, 0, reshade::d3d12::convert_format(Format)))
		return;
#endif
	_orig->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology)
{
	wwmi::binding_trace::topology(_orig, 1, 1005, PrimitiveTopology);

#if RESHADE_ADDON >= 2
	if (!reshade::has_addon_event<reshade::addon_event::bind_pipeline_states>())
		return;

	const reshade::api::dynamic_state states[1] = { reshade::api::dynamic_state::primitive_topology };
	const uint32_t values[1] = { static_cast<uint32_t>(reshade::d3d12::convert_primitive_topology(PrimitiveTopology)) };

	reshade::invoke_addon_event<reshade::addon_event::bind_pipeline_states>(this, static_cast<uint32_t>(std::size(states)), states, values);
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT *pViewports)
{
	_orig->RSSetViewports(NumViewports, pViewports);

#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::bind_viewports>(this, 0, NumViewports, reinterpret_cast<const reshade::api::viewport *>(pViewports));
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::RSSetScissorRects(UINT NumRects, const D3D12_RECT *pRects)
{
	_orig->RSSetScissorRects(NumRects, pRects);

#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::bind_scissor_rects>(this, 0, NumRects, reinterpret_cast<const reshade::api::rect *>(pRects));
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::OMSetBlendFactor(const FLOAT BlendFactor[4])
{
	_orig->OMSetBlendFactor(BlendFactor);

#if RESHADE_ADDON >= 2
	if (!reshade::has_addon_event<reshade::addon_event::bind_pipeline_states>())
		return;

	const reshade::api::dynamic_state states[1] = { reshade::api::dynamic_state::blend_constant };
	const uint32_t values[1] = {
		(BlendFactor == nullptr) ? 0xFFFFFFFF : // Default blend factor is { 1, 1, 1, 1 }, see D3D12_DEFAULT_BLEND_FACTOR_*
			((static_cast<uint32_t>(BlendFactor[0] * 255.f) & 0xFF)) |
			((static_cast<uint32_t>(BlendFactor[1] * 255.f) & 0xFF) << 8) |
			((static_cast<uint32_t>(BlendFactor[2] * 255.f) & 0xFF) << 16) |
			((static_cast<uint32_t>(BlendFactor[3] * 255.f) & 0xFF) << 24) };

	reshade::invoke_addon_event<reshade::addon_event::bind_pipeline_states>(this, static_cast<uint32_t>(std::size(states)), states, values);
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::OMSetStencilRef(UINT StencilRef)
{
	_orig->OMSetStencilRef(StencilRef);

#if RESHADE_ADDON >= 2
	if (!reshade::has_addon_event<reshade::addon_event::bind_pipeline_states>())
		return;

	const reshade::api::dynamic_state states[2] = { reshade::api::dynamic_state::front_stencil_reference_value, reshade::api::dynamic_state::back_stencil_reference_value };
	const uint32_t values[2] = { StencilRef, StencilRef };

	reshade::invoke_addon_event<reshade::addon_event::bind_pipeline_states>(this, static_cast<uint32_t>(std::size(states)), states, values);
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetPipelineState(ID3D12PipelineState *pPipelineState)
{
	_orig->SetPipelineState(pPipelineState);
	wcs::observe(_wwmi_compute, [&](auto &v){v.pipeline(PipelineKind::ordinary, reinterpret_cast<uint64_t>(pPipelineState));});

#if RESHADE_ADDON >= 2
	reshade::invoke_addon_event<reshade::addon_event::bind_pipeline>(this, reshade::api::pipeline_stage::all, to_handle(pPipelineState));
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER *pBarriers)
{
	wwmi::binding_trace::barriers(_orig, 1, 1006, NumBarriers, pBarriers);
	wwmi::native_buffer::legacy(native_buffer_recording_id(this),NumBarriers,pBarriers);
	for(UINT i=0;i<NumBarriers;++i)if(pBarriers[i].Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING)
		wwmi::rt_forward::buffer(this,_orig,WWMI_RT_BUFFER_UNKNOWN,WWMI_RT_BUFFER_BEFORE_NATIVE,nullptr,nullptr,0,0,0,nullptr);

#if RESHADE_ADDON >= 2
	if (!reshade::has_addon_event<reshade::addon_event::barrier>())
		return;

	temp_mem<reshade::api::resource, 32> resources(NumBarriers);
	temp_mem<reshade::api::resource_usage, 32> old_state(NumBarriers), new_state(NumBarriers);
	for (UINT i = 0; i < NumBarriers; ++i)
	{
		switch (pBarriers[i].Type)
		{
		case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
			resources[i] = to_handle(pBarriers[i].Transition.pResource);
			old_state[i] = reshade::d3d12::convert_resource_states_to_usage(pBarriers[i].Transition.StateBefore);
			new_state[i] = reshade::d3d12::convert_resource_states_to_usage(pBarriers[i].Transition.StateAfter);
			break;
		case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
			resources[i] = to_handle(pBarriers[i].Aliasing.pResourceAfter);
			old_state[i] = reshade::api::resource_usage::undefined;
			new_state[i] = reshade::api::resource_usage::general;
			break;
		case D3D12_RESOURCE_BARRIER_TYPE_UAV:
			resources[i] = to_handle(pBarriers[i].UAV.pResource);
			old_state[i] = reshade::api::resource_usage::unordered_access;
			new_state[i] = reshade::api::resource_usage::unordered_access;
			break;
		}
	}

	reshade::invoke_addon_event<reshade::addon_event::barrier>(this, NumBarriers, resources.p, old_state.p, new_state.p);
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::ExecuteBundle(ID3D12GraphicsCommandList *pCommandList)
{
	assert(pCommandList != nullptr);

	// Get original command list pointer from proxy object
	const auto command_list_proxy = static_cast<D3D12GraphicsCommandList *>(pCommandList);

#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::execute_secondary_command_list>(this, command_list_proxy);
#endif

	wwmi::binding_trace::bundle(_orig, 1, 1007, command_list_proxy->_orig);
	wwmi::native_buffer::opaque(native_buffer_recording_id(this));
	wwmi::rt_forward::buffer(this,_orig,WWMI_RT_BUFFER_UNKNOWN,WWMI_RT_BUFFER_BEFORE_NATIVE,nullptr,nullptr,0,0,0,nullptr);
	wcs::unobserved(_wwmi_compute, false); // Bundle bindings are opaque, not Begin/EndRenderPass.
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetDescriptorHeaps(UINT NumDescriptorHeaps, ID3D12DescriptorHeap *const *ppDescriptorHeaps)
{
	assert(NumDescriptorHeaps <= 2);

#if RESHADE_ADDON >= 2
	const auto wrapped_heaps = ppDescriptorHeaps;
	temp_mem<ID3D12DescriptorHeap *, 2> heaps(NumDescriptorHeaps);
	for (UINT i = 0; i < NumDescriptorHeaps; ++i)
		heaps[i] = static_cast<D3D12DescriptorHeap *>(ppDescriptorHeaps[i])->_orig;
	ppDescriptorHeaps = heaps.p;
#endif

	_orig->SetDescriptorHeaps(NumDescriptorHeaps, ppDescriptorHeaps);
	wcs::heaps(_wwmi_compute, [&](uint64_t (&next)[2], uint64_t (&tags)[2]) {
		bool known = true;
		for (UINT i = 0; i < NumDescriptorHeaps; ++i)
		{
#if RESHADE_ADDON >= 2
			const auto tagged = static_cast<D3D12DescriptorHeap *>(wrapped_heaps[i])->_internal_base_cpu_handle;
			const auto type = tagged.ptr & 0x3;
			if (type <= D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
			{
				known &= next[type] == 0;
				next[type] = _device->wwmi_heap_generation(tagged);
				known &= next[type] != 0;
				tags[type] = tagged.ptr;
			}
			else
				known = false;
#else
			const auto type = ppDescriptorHeaps[i]->GetDesc().Type;
			if (type <= D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
			{
				known &= next[type] == 0;
				next[type] = reinterpret_cast<uint64_t>(ppDescriptorHeaps[i]);
				tags[type] = next[type];
			}
			else
				known = false;
#endif
		}
		return known;
	});

	for (UINT i = 0; i < 2; ++i)
	{
		_current_descriptor_heaps[i] = i < NumDescriptorHeaps ? ppDescriptorHeaps[i] : nullptr;
#if RESHADE_ADDON >= 2
		_previous_descriptor_heaps[i] = _current_descriptor_heaps[i];
#endif
	}
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetComputeRootSignature(ID3D12RootSignature *pRootSignature)
{
	_orig->SetComputeRootSignature(pRootSignature);
	wcs::signature(_wwmi_compute, reinterpret_cast<uint64_t>(pRootSignature));

	_current_root_signature[1] = pRootSignature;

#if RESHADE_ADDON >= 2
	reshade::invoke_addon_event<reshade::addon_event::bind_descriptor_tables>(
		this,
		reshade::api::shader_stage::all_compute | reshade::api::shader_stage::all_ray_tracing,
		to_handle(_current_root_signature[1]),
		0,
		0, nullptr);
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetGraphicsRootSignature(ID3D12RootSignature *pRootSignature)
{
	_orig->SetGraphicsRootSignature(pRootSignature);
	wcs::graphics_signature(_wwmi_compute, reinterpret_cast<uint64_t>(pRootSignature));

	_current_root_signature[0] = pRootSignature;

#if RESHADE_ADDON >= 2
	reshade::invoke_addon_event<reshade::addon_event::bind_descriptor_tables>(
		this,
		reshade::api::shader_stage::all_graphics,
		to_handle(_current_root_signature[0]),
		0,
		0, nullptr);
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetComputeRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
	_orig->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor);
	wcs::binding(_wwmi_compute, RootKind::table, RootParameterIndex, BaseDescriptor.ptr);

#if RESHADE_ADDON >= 2
	reshade::invoke_addon_event<reshade::addon_event::bind_descriptor_tables>(
		this,
		reshade::api::shader_stage::all_compute | reshade::api::shader_stage::all_ray_tracing,
		to_handle(_current_root_signature[1]),
		RootParameterIndex,
		1, reinterpret_cast<const reshade::api::descriptor_table *>(&BaseDescriptor));
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetGraphicsRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
	_orig->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor);
	wcs::graphics_table(_wwmi_compute, RootParameterIndex, BaseDescriptor.ptr);

#if RESHADE_ADDON >= 2
	reshade::invoke_addon_event<reshade::addon_event::bind_descriptor_tables>(
		this,
		reshade::api::shader_stage::all_graphics,
		to_handle(_current_root_signature[0]),
		RootParameterIndex,
		1, reinterpret_cast<const reshade::api::descriptor_table *>(&BaseDescriptor));
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetComputeRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues)
{
	_orig->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
	wcs::constants(_wwmi_compute, RootParameterIndex, DestOffsetIn32BitValues, 1, &SrcData);

#if RESHADE_ADDON >= 2
	reshade::invoke_addon_event<reshade::addon_event::push_constants>(
		this,
		reshade::api::shader_stage::all_compute | reshade::api::shader_stage::all_ray_tracing,
		to_handle(_current_root_signature[1]),
		RootParameterIndex,
		DestOffsetIn32BitValues,
		1, &SrcData);
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetGraphicsRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues)
{
	_orig->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);

#if RESHADE_ADDON >= 2
	reshade::invoke_addon_event<reshade::addon_event::push_constants>(
		this,
		reshade::api::shader_stage::all_graphics,
		to_handle(_current_root_signature[0]),
		RootParameterIndex,
		DestOffsetIn32BitValues,
		1, &SrcData);
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetComputeRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void *pSrcData, UINT DestOffsetIn32BitValues)
{
	_orig->SetComputeRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);
	wcs::constants(_wwmi_compute, RootParameterIndex, DestOffsetIn32BitValues, Num32BitValuesToSet, static_cast<const uint32_t *>(pSrcData));

#if RESHADE_ADDON >= 2
	reshade::invoke_addon_event<reshade::addon_event::push_constants>(
		this,
		reshade::api::shader_stage::all_compute | reshade::api::shader_stage::all_ray_tracing,
		to_handle(_current_root_signature[1]),
		RootParameterIndex,
		DestOffsetIn32BitValues,
		Num32BitValuesToSet, static_cast<const uint32_t *>(pSrcData));
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetGraphicsRoot32BitConstants(UINT RootParameterIndex, UINT Num32BitValuesToSet, const void *pSrcData, UINT DestOffsetIn32BitValues)
{
	_orig->SetGraphicsRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues);

#if RESHADE_ADDON >= 2
	reshade::invoke_addon_event<reshade::addon_event::push_constants>(
		this,
		reshade::api::shader_stage::all_graphics,
		to_handle(_current_root_signature[0]),
		RootParameterIndex,
		DestOffsetIn32BitValues,
		Num32BitValuesToSet, static_cast<const uint32_t *>(pSrcData));
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetComputeRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
	_orig->SetComputeRootConstantBufferView(RootParameterIndex, BufferLocation);
	wcs::binding(_wwmi_compute, RootKind::cbv, RootParameterIndex, BufferLocation);

#if RESHADE_ADDON >= 2
	if (!reshade::has_addon_event<reshade::addon_event::push_descriptors>())
		return;

	reshade::api::buffer_range buffer_range;
	if (!_device->resolve_gpu_address(BufferLocation, &buffer_range.buffer, &buffer_range.offset))
		return;
	buffer_range.size = UINT64_MAX;

	reshade::invoke_addon_event<reshade::addon_event::push_descriptors>(
		this,
		reshade::api::shader_stage::all_compute | reshade::api::shader_stage::all_ray_tracing,
		to_handle(_current_root_signature[1]),
		RootParameterIndex,
		reshade::api::descriptor_table_update { {}, 0, 0, 1, reshade::api::descriptor_type::constant_buffer, &buffer_range });
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetGraphicsRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
	_orig->SetGraphicsRootConstantBufferView(RootParameterIndex, BufferLocation);

#if RESHADE_ADDON >= 2
	if (!reshade::has_addon_event<reshade::addon_event::push_descriptors>())
		return;

	reshade::api::buffer_range buffer_range;
	if (!_device->resolve_gpu_address(BufferLocation, &buffer_range.buffer, &buffer_range.offset))
		return;
	buffer_range.size = UINT64_MAX;

	reshade::invoke_addon_event<reshade::addon_event::push_descriptors>(
		this,
		reshade::api::shader_stage::all_graphics,
		to_handle(_current_root_signature[0]),
		RootParameterIndex,
		reshade::api::descriptor_table_update { {}, 0, 0, 1, reshade::api::descriptor_type::constant_buffer, &buffer_range });
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetComputeRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
	_orig->SetComputeRootShaderResourceView(RootParameterIndex, BufferLocation);
	wcs::binding(_wwmi_compute, RootKind::srv, RootParameterIndex, BufferLocation);

#if RESHADE_ADDON >= 2
	if (!reshade::has_addon_event<reshade::addon_event::push_descriptors>())
		return;

	reshade::api::buffer_range buffer_range;
	bool acceleration_structure;
	if (!_device->resolve_gpu_address(BufferLocation, &buffer_range.buffer, &buffer_range.offset, &acceleration_structure))
		return;
	buffer_range.size = UINT64_MAX;

	// TODO: Get a working 'resource_view' handle for the SRV that can be passed as descriptor update, when it is not an acceleration structure
	reshade::invoke_addon_event<reshade::addon_event::push_descriptors>(
		this,
		reshade::api::shader_stage::all_compute | reshade::api::shader_stage::all_ray_tracing,
		to_handle(_current_root_signature[1]),
		RootParameterIndex,
		reshade::api::descriptor_table_update { {}, 0, 0, 1, acceleration_structure ? reshade::api::descriptor_type::acceleration_structure : reshade::api::descriptor_type::buffer_shader_resource_view, &BufferLocation });
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetGraphicsRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
	_orig->SetGraphicsRootShaderResourceView(RootParameterIndex, BufferLocation);

#if RESHADE_ADDON >= 2
	// TODO: Get a working 'resource_view' handle for the SRV that can be passed as descriptor update
	reshade::invoke_addon_event<reshade::addon_event::push_descriptors>(
		this,
		reshade::api::shader_stage::all_graphics,
		to_handle(_current_root_signature[0]),
		RootParameterIndex,
		reshade::api::descriptor_table_update { {}, 0, 0, 1, reshade::api::descriptor_type::buffer_shader_resource_view, &BufferLocation });
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetComputeRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
	_orig->SetComputeRootUnorderedAccessView(RootParameterIndex, BufferLocation);
	wcs::binding(_wwmi_compute, RootKind::uav, RootParameterIndex, BufferLocation);

#if RESHADE_ADDON >= 2
	// TODO: Get a working 'resource_view' handle for the UAV that can be passed as descriptor update
	reshade::invoke_addon_event<reshade::addon_event::push_descriptors>(
		this,
		reshade::api::shader_stage::all_compute | reshade::api::shader_stage::all_ray_tracing,
		to_handle(_current_root_signature[1]),
		RootParameterIndex,
		reshade::api::descriptor_table_update { {}, 0, 0, 1, reshade::api::descriptor_type::buffer_unordered_access_view, &BufferLocation });
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
	_orig->SetGraphicsRootUnorderedAccessView(RootParameterIndex, BufferLocation);

#if RESHADE_ADDON >= 2
	// TODO: Get a working 'resource_view' handle for the UAV that can be passed as descriptor update
	reshade::invoke_addon_event<reshade::addon_event::push_descriptors>(
		this,
		reshade::api::shader_stage::all_graphics,
		to_handle(_current_root_signature[0]),
		RootParameterIndex,
		reshade::api::descriptor_table_update { {}, 0, 0, 1, reshade::api::descriptor_type::buffer_unordered_access_view, &BufferLocation });
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *pView)
{
	wwmi::binding_trace::indices(_orig, 1, 1008, pView);

#if RESHADE_ADDON >= 2
	if (!reshade::has_addon_event<reshade::addon_event::bind_index_buffer>())
		return;

	reshade::api::resource buffer = { 0 };
	uint64_t offset = 0;
	uint32_t index_size = 0;

	if (pView != nullptr)
	{
		if (!_device->resolve_gpu_address(pView->BufferLocation, &buffer, &offset))
			return;
		index_size = pView->Format == DXGI_FORMAT_R16_UINT ? 2 : 4;
	}

	reshade::invoke_addon_event<reshade::addon_event::bind_index_buffer>(this, buffer, offset, index_size);
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::IASetVertexBuffers(UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW *pViews)
{
	wwmi::binding_trace::vertices(_orig, 1, 1009, StartSlot, NumViews, pViews);

#if RESHADE_ADDON >= 2
	if (!reshade::has_addon_event<reshade::addon_event::bind_vertex_buffers>())
		return;

	temp_mem<reshade::api::resource, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> buffers(NumViews);
	temp_mem<uint64_t, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> offsets(NumViews);
	temp_mem<uint32_t, D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> strides(NumViews);
	for (UINT i = 0; i < NumViews; ++i)
	{
		if (pViews != nullptr)
		{
			if (!_device->resolve_gpu_address(pViews[i].BufferLocation, &buffers[i], &offsets[i]))
				return;
			strides[i] = pViews[i].StrideInBytes;
		}
		else
		{
			buffers[i] = { 0 };
			offsets[i] = strides[i] = 0;
		}
	}

	reshade::invoke_addon_event<reshade::addon_event::bind_vertex_buffers>(this, StartSlot, NumViews, buffers.p, offsets.p, strides.p);
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SOSetTargets(UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *pViews)
{
	_orig->SOSetTargets(StartSlot, NumViews, pViews);

#if RESHADE_ADDON >= 2
	if (!reshade::has_addon_event<reshade::addon_event::bind_stream_output_buffers>())
		return;

	temp_mem<reshade::api::resource, D3D12_SO_BUFFER_SLOT_COUNT> buffers(NumViews);
	temp_mem<uint64_t, D3D12_SO_BUFFER_SLOT_COUNT> offsets(NumViews);
	temp_mem<uint64_t, D3D12_SO_BUFFER_SLOT_COUNT> max_sizes(NumViews);
	temp_mem<reshade::api::resource, D3D12_SO_BUFFER_SLOT_COUNT> counter_buffers(NumViews);
	temp_mem<uint64_t, D3D12_SO_BUFFER_SLOT_COUNT> counter_offsets(NumViews);
	for (UINT i = 0; i < NumViews; ++i)
	{
		if (pViews != nullptr && pViews[i].SizeInBytes != 0)
		{
			if (!_device->resolve_gpu_address(pViews[i].BufferLocation, &buffers[i], &offsets[i]))
				return;
			max_sizes[i] = pViews[i].SizeInBytes;
			if (!_device->resolve_gpu_address(pViews[i].BufferFilledSizeLocation, &counter_buffers[i], &counter_offsets[i]))
				return;
		}
		else
		{
			buffers[i] = counter_buffers[i] = { 0 };
			offsets[i] = counter_offsets[i] = max_sizes[i] = 0;
		}
	}

	reshade::invoke_addon_event<reshade::addon_event::bind_stream_output_buffers>(this, StartSlot, NumViews, buffers.p, offsets.p, max_sizes.p, counter_buffers.p, counter_offsets.p);
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::OMSetRenderTargets(UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE *pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, D3D12_CPU_DESCRIPTOR_HANDLE const *pDepthStencilDescriptor)
{
	_orig->OMSetRenderTargets(NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);

#if RESHADE_ADDON
	if (!reshade::has_addon_event<reshade::addon_event::bind_render_targets_and_depth_stencil>())
		return;

	temp_mem<reshade::api::resource_view, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs(NumRenderTargetDescriptors);
	for (UINT i = 0; i < NumRenderTargetDescriptors; ++i)
		rtvs[i] = to_handle(RTsSingleHandleToDescriptorRange ? _device->offset_descriptor_handle(*pRenderTargetDescriptors, i, D3D12_DESCRIPTOR_HEAP_TYPE_RTV) : pRenderTargetDescriptors[i]);

	reshade::invoke_addon_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(this, NumRenderTargetDescriptors, rtvs.p, to_handle(pDepthStencilDescriptor != nullptr ? *pDepthStencilDescriptor : D3D12_CPU_DESCRIPTOR_HANDLE {}));
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, D3D12_CLEAR_FLAGS ClearFlags, FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT *pRects)
{
#if RESHADE_ADDON
	if (reshade::invoke_addon_event<reshade::addon_event::clear_depth_stencil_view>(this, to_handle(DepthStencilView), (ClearFlags & D3D12_CLEAR_FLAG_DEPTH) ? &Depth : nullptr, (ClearFlags & D3D12_CLEAR_FLAG_STENCIL) ? &Stencil : nullptr, NumRects, reinterpret_cast<const reshade::api::rect *>(pRects)))
		return;
#endif
	_orig->ClearDepthStencilView(DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, const FLOAT ColorRGBA[4], UINT NumRects, const D3D12_RECT *pRects)
{
#if RESHADE_ADDON
	if (reshade::invoke_addon_event<reshade::addon_event::clear_render_target_view>(this, to_handle(RenderTargetView), ColorRGBA, NumRects, reinterpret_cast<const reshade::api::rect *>(pRects)))
		return;
#endif
	_orig->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource *pResource, const UINT Values[4], UINT NumRects, const D3D12_RECT *pRects)
{
#if RESHADE_ADDON >= 2
	ViewCPUHandle = _device->convert_to_original_cpu_descriptor_handle(ViewCPUHandle);
#endif

#if RESHADE_ADDON
	if (reshade::invoke_addon_event<reshade::addon_event::clear_unordered_access_view_uint>(this, to_handle(ViewCPUHandle), Values, NumRects, reinterpret_cast<const reshade::api::rect *>(pRects)))
		return;
#endif
	if(pResource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER)wwmi::rt_forward::buffer(this,_orig,WWMI_RT_BUFFER_WRITE,WWMI_RT_BUFFER_BEFORE_NATIVE,nullptr,pResource,0,0,pResource->GetDesc().Width,nullptr);
	_orig->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource *pResource, const FLOAT Values[4], UINT NumRects, const D3D12_RECT *pRects)
{
#if RESHADE_ADDON >= 2
	ViewCPUHandle = _device->convert_to_original_cpu_descriptor_handle(ViewCPUHandle);
#endif

#if RESHADE_ADDON
	if (reshade::invoke_addon_event<reshade::addon_event::clear_unordered_access_view_float>(this, to_handle(ViewCPUHandle), Values, NumRects, reinterpret_cast<const reshade::api::rect *>(pRects)))
		return;
#endif
	if(pResource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER)wwmi::rt_forward::buffer(this,_orig,WWMI_RT_BUFFER_WRITE,WWMI_RT_BUFFER_BEFORE_NATIVE,nullptr,pResource,0,0,pResource->GetDesc().Width,nullptr);
	_orig->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::DiscardResource(ID3D12Resource *pResource, const D3D12_DISCARD_REGION *pRegion)
{
	_orig->DiscardResource(pResource, pRegion);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::BeginQuery(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index)
{
#if RESHADE_ADDON >= 2
	if (reshade::invoke_addon_event<reshade::addon_event::begin_query>(this, to_handle(pQueryHeap), reshade::d3d12::convert_query_type(Type), Index))
		return;
#endif
	_orig->BeginQuery(pQueryHeap, Type, Index);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::EndQuery(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index)
{
#if RESHADE_ADDON >= 2
	if (reshade::invoke_addon_event<reshade::addon_event::end_query>(this, to_handle(pQueryHeap), reshade::d3d12::convert_query_type(Type), Index))
		return;
#endif
	_orig->EndQuery(pQueryHeap, Type, Index);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::ResolveQueryData(ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT StartIndex, UINT NumQueries, ID3D12Resource *pDestinationBuffer, UINT64 AlignedDestinationBufferOffset)
{
#if RESHADE_ADDON >= 2
	if (const reshade::api::query_type query_type = reshade::d3d12::convert_query_type(Type);
		reshade::invoke_addon_event<reshade::addon_event::copy_query_heap_results>(this, to_handle(pQueryHeap), query_type, StartIndex, NumQueries, to_handle(pDestinationBuffer), AlignedDestinationBufferOffset, reshade::d3d12::get_query_size(query_type).first))
		return;
#endif
	wwmi::rt_forward::buffer(this,_orig,WWMI_RT_BUFFER_WRITE,WWMI_RT_BUFFER_BEFORE_NATIVE,nullptr,pDestinationBuffer,0,AlignedDestinationBufferOffset,uint64_t(NumQueries)*reshade::d3d12::get_query_size(reshade::d3d12::convert_query_type(Type)).first,nullptr);
	_orig->ResolveQueryData(pQueryHeap, Type, StartIndex, NumQueries, pDestinationBuffer, AlignedDestinationBufferOffset);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetPredication(ID3D12Resource *pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Operation)
{
	wwmi::binding_trace::predicate(_orig, 1, 1010, pBuffer, AlignedBufferOffset, Operation);
	wwmi::native_buffer::predicate(native_buffer_recording_id(this),pBuffer==nullptr);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetMarker(UINT Metadata, const void *pData, UINT Size)
{
	_orig->SetMarker(Metadata, pData, Size);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::BeginEvent(UINT Metadata, const void *pData, UINT Size)
{
	_orig->BeginEvent(Metadata, pData, Size);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::EndEvent()
{
	_orig->EndEvent();
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::ExecuteIndirect(ID3D12CommandSignature *pCommandSignature, UINT MaxCommandCount, ID3D12Resource *pArgumentBuffer, UINT64 ArgumentBufferOffset, ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset)
{
#if RESHADE_ADDON
	if (reshade::invoke_addon_event<reshade::addon_event::draw_or_dispatch_indirect>(this, reshade::api::indirect_command::unknown, to_handle(pArgumentBuffer), ArgumentBufferOffset, MaxCommandCount, 0))
		return;
#endif
	wwmi::binding_trace::indirect(_orig, 1, 1011, pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset);
	wcs::indirect(_wwmi_compute);
}

void STDMETHODCALLTYPE D3D12GraphicsCommandList::AtomicCopyBufferUINT(ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT Dependencies, ID3D12Resource *const *ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64 *pDependentSubresourceRanges)
{
	assert(_interface_version >= 1);

	static_cast<ID3D12GraphicsCommandList1 *>(_orig)->AtomicCopyBufferUINT(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, Dependencies, ppDependentResources, pDependentSubresourceRanges);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::AtomicCopyBufferUINT64(ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT Dependencies, ID3D12Resource *const *ppDependentResources, const D3D12_SUBRESOURCE_RANGE_UINT64 *pDependentSubresourceRanges)
{
	assert(_interface_version >= 1);

	static_cast<ID3D12GraphicsCommandList1 *>(_orig)->AtomicCopyBufferUINT64(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, Dependencies, ppDependentResources, pDependentSubresourceRanges);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::OMSetDepthBounds(FLOAT Min, FLOAT Max)
{
	assert(_interface_version >= 1);

	static_cast<ID3D12GraphicsCommandList1 *>(_orig)->OMSetDepthBounds(Min, Max);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetSamplePositions(UINT NumSamplesPerPixel, UINT NumPixels, D3D12_SAMPLE_POSITION *pSamplePositions)
{
	assert(_interface_version >= 1);

	static_cast<ID3D12GraphicsCommandList1 *>(_orig)->SetSamplePositions(NumSamplesPerPixel, NumPixels, pSamplePositions);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::ResolveSubresourceRegion(ID3D12Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, ID3D12Resource *pSrcResource, UINT SrcSubresource, D3D12_RECT *pSrcRect, DXGI_FORMAT Format, D3D12_RESOLVE_MODE ResolveMode)
{
	assert(_interface_version >= 1);

#if RESHADE_ADDON >= 2
	assert(pDstResource != nullptr && pSrcResource != nullptr);

	const bool use_src_box = pSrcRect != nullptr;
	reshade::api::subresource_box src_box;

	if (use_src_box)
	{
		src_box.left = static_cast<uint32_t>(pSrcRect->left);
		src_box.top = static_cast<uint32_t>(pSrcRect->top);
		src_box.front = 0;
		src_box.right = static_cast<uint32_t>(pSrcRect->right);
		src_box.bottom = static_cast<uint32_t>(pSrcRect->bottom);
		src_box.back = 1;
	}

	if (reshade::invoke_addon_event<reshade::addon_event::resolve_texture_region>(this, to_handle(pSrcResource), SrcSubresource, use_src_box ? &src_box : nullptr, to_handle(pDstResource), DstSubresource, DstX, DstY, 0, reshade::d3d12::convert_format(Format)))
		return;
#endif

	static_cast<ID3D12GraphicsCommandList1 *>(_orig)->ResolveSubresourceRegion(pDstResource, DstSubresource, DstX, DstY, pSrcResource, SrcSubresource, pSrcRect, Format, ResolveMode);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetViewInstanceMask(UINT Mask)
{
	assert(_interface_version >= 1);

	static_cast<ID3D12GraphicsCommandList1 *>(_orig)->SetViewInstanceMask(Mask);
}

void STDMETHODCALLTYPE D3D12GraphicsCommandList::WriteBufferImmediate(UINT Count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *pParams, const D3D12_WRITEBUFFERIMMEDIATE_MODE *pModes)
{
	assert(_interface_version >= 2);

	static_cast<ID3D12GraphicsCommandList2 *>(_orig)->WriteBufferImmediate(Count, pParams, pModes);
}

void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetProtectedResourceSession(ID3D12ProtectedResourceSession *pProtectedResourceSession)
{
	assert(_interface_version >= 3);

	static_cast<ID3D12GraphicsCommandList3 *>(_orig)->SetProtectedResourceSession(pProtectedResourceSession);
}

void STDMETHODCALLTYPE D3D12GraphicsCommandList::BeginRenderPass(UINT NumRenderTargets, const D3D12_RENDER_PASS_RENDER_TARGET_DESC *pRenderTargets, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *pDepthStencil, D3D12_RENDER_PASS_FLAGS Flags)
{
	assert(_interface_version >= 4);

#if RESHADE_ADDON
	temp_mem<reshade::api::render_pass_render_target_desc, D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT> rts(NumRenderTargets);
	for (UINT i = 0; i < NumRenderTargets; ++i)
		rts[i] = reshade::d3d12::convert_render_pass_render_target_desc(pRenderTargets[i]);

	reshade::api::render_pass_depth_stencil_desc ds;
	if (pDepthStencil != nullptr)
		ds = reshade::d3d12::convert_render_pass_depth_stencil_desc(*pDepthStencil);

	if (reshade::invoke_addon_event<reshade::addon_event::begin_render_pass>(this, NumRenderTargets, rts.p, pDepthStencil != nullptr ? &ds : nullptr, reshade::d3d12::convert_render_pass_flags(Flags)))
		return;
#endif

	static_cast<ID3D12GraphicsCommandList4 *>(_orig)->BeginRenderPass(NumRenderTargets, pRenderTargets, pDepthStencil, Flags);
	wcs::begin_render_pass(_wwmi_compute, Flags);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::EndRenderPass()
{
	assert(_interface_version >= 4);

#if RESHADE_ADDON
	if (reshade::invoke_addon_event<reshade::addon_event::end_render_pass>(this))
		return;
#endif

	static_cast<ID3D12GraphicsCommandList4 *>(_orig)->EndRenderPass();
	wcs::end_render_pass(_wwmi_compute);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::InitializeMetaCommand(ID3D12MetaCommand *pMetaCommand, const void *pInitializationParametersData, SIZE_T InitializationParametersDataSizeInBytes)
{
	assert(_interface_version >= 4);

	static_cast<ID3D12GraphicsCommandList4 *>(_orig)->InitializeMetaCommand(pMetaCommand, pInitializationParametersData, InitializationParametersDataSizeInBytes);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::ExecuteMetaCommand(ID3D12MetaCommand *pMetaCommand, const void *pExecutionParametersData, SIZE_T ExecutionParametersDataSizeInBytes)
{
	assert(_interface_version >= 4);

	static_cast<ID3D12GraphicsCommandList4 *>(_orig)->ExecuteMetaCommand(pMetaCommand, pExecutionParametersData, ExecutionParametersDataSizeInBytes);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::BuildRaytracingAccelerationStructure(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *pDesc, UINT NumPostbuildInfoDescs, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *pPostbuildInfoDescs)
{
	assert(_interface_version >= 4);

#if RESHADE_ADDON >= 2
	assert(pDesc != nullptr);

	if (reshade::has_addon_event<reshade::addon_event::build_acceleration_structure>())
	{
		std::vector<reshade::api::acceleration_structure_build_input> build_inputs;
		if (pDesc->Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL)
		{
			build_inputs.push_back({ reshade::api::resource {}, pDesc->Inputs.InstanceDescs, pDesc->Inputs.NumDescs, pDesc->Inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS });
		}
		else
		{
			build_inputs.reserve(pDesc->Inputs.NumDescs);
			for (UINT k = 0; k < pDesc->Inputs.NumDescs; ++k)
				build_inputs.push_back(reshade::d3d12::convert_acceleration_structure_build_input(pDesc->Inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS ? *pDesc->Inputs.ppGeometryDescs[k] : pDesc->Inputs.pGeometryDescs[k]));
		}

		if (reshade::invoke_addon_event<reshade::addon_event::build_acceleration_structure>(
				this,
				reshade::d3d12::convert_acceleration_structure_type(pDesc->Inputs.Type),
				reshade::d3d12::convert_acceleration_structure_build_flags(pDesc->Inputs.Flags),
				static_cast<uint32_t>(build_inputs.size()),
				build_inputs.data(),
				reshade::api::resource {},
				pDesc->ScratchAccelerationStructureData,
				reshade::api::resource_view { pDesc->SourceAccelerationStructureData },
				reshade::api::resource_view { pDesc->DestAccelerationStructureData },
				(pDesc->Inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) != 0 ? reshade::api::acceleration_structure_build_mode::update : reshade::api::acceleration_structure_build_mode::build))
			return;
	}
#endif

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC wwmi_desc;
	if (pDesc->Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL &&
		pDesc->Inputs.DescsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY && wwmi::rt_forward::enabled())
	{
		if (const uint64_t input = wwmi::rt_forward::tlas_input(this, _orig, pDesc))
		{
			wwmi_desc = *pDesc;
			wwmi_desc.Inputs.InstanceDescs = input;
			pDesc = &wwmi_desc;
		}
	}
	if(pDesc->Inputs.Type==D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL)
		wwmi::rt_forward::buffer(this,_orig,WWMI_RT_BUFFER_BLAS,WWMI_RT_BUFFER_BEFORE_NATIVE,nullptr,nullptr,0,0,0,pDesc);
	const auto wwmi_trace = wwmi::native_trace::build_as_before(this, _orig, pDesc, NumPostbuildInfoDescs, pPostbuildInfoDescs);
	static_cast<ID3D12GraphicsCommandList4 *>(_orig)->BuildRaytracingAccelerationStructure(pDesc, NumPostbuildInfoDescs, pPostbuildInfoDescs);
	wwmi::native_trace::call_after(wwmi_trace, "build_as", this);
	if(pDesc->Inputs.Type==D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL)
		wwmi::rt_forward::buffer(this,_orig,WWMI_RT_BUFFER_BLAS,WWMI_RT_BUFFER_NATIVE_RECORDED,nullptr,nullptr,0,0,0,pDesc);
	if (pDesc->Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL)
		wwmi::rt_forward::blas_after(this, _orig, pDesc);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::EmitRaytracingAccelerationStructurePostbuildInfo(const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *pDesc, UINT NumSourceAccelerationStructures, const D3D12_GPU_VIRTUAL_ADDRESS *pSourceAccelerationStructureData)
{
	assert(_interface_version >= 4);

#if RESHADE_ADDON >= 2
	assert(pDesc != nullptr);

	if (reshade::has_addon_event<reshade::addon_event::query_acceleration_structures>())
	{
		reshade::api::resource buffer = {};
		uint64_t offset = 0;
		if (_device->resolve_gpu_address(pDesc->DestBuffer, &buffer, &offset))
		{
			if (const reshade::api::query_type query_type = reshade::d3d12::convert_acceleration_structure_post_build_info_type(pDesc->InfoType);
				reshade::invoke_addon_event<reshade::addon_event::query_acceleration_structures>(
					this,
					NumSourceAccelerationStructures,
					reinterpret_cast<const reshade::api::resource_view *>(pSourceAccelerationStructureData),
					reshade::api::query_heap { buffer.handle },
					query_type,
					static_cast<uint32_t>(offset / reshade::d3d12::get_query_size(query_type).first)))
				return;
		}
	}
#endif

	const auto wwmi_trace = wwmi::native_trace::postbuild_before(this, _orig, pDesc, NumSourceAccelerationStructures, pSourceAccelerationStructureData);
	static_cast<ID3D12GraphicsCommandList4 *>(_orig)->EmitRaytracingAccelerationStructurePostbuildInfo(pDesc, NumSourceAccelerationStructures, pSourceAccelerationStructureData);
	wwmi::native_trace::call_after(wwmi_trace, "postbuild_info", this);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::CopyRaytracingAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS DestAccelerationStructureData, D3D12_GPU_VIRTUAL_ADDRESS SourceAccelerationStructureData, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode)
{
	assert(_interface_version >= 4);
#if RESHADE_ADDON >= 2
	if (reshade::invoke_addon_event<reshade::addon_event::copy_acceleration_structure>(
			this,
			reshade::api::resource_view { SourceAccelerationStructureData },
			reshade::api::resource_view { DestAccelerationStructureData },
			reshade::d3d12::convert_acceleration_structure_copy_mode(Mode)))
		return;
#endif

	const auto wwmi_trace = wwmi::native_trace::copy_as_before(this, _orig, DestAccelerationStructureData, SourceAccelerationStructureData, Mode);
	static_cast<ID3D12GraphicsCommandList4 *>(_orig)->CopyRaytracingAccelerationStructure(DestAccelerationStructureData, SourceAccelerationStructureData, Mode);
	wwmi::native_trace::call_after(wwmi_trace, "copy_as", this);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::SetPipelineState1(ID3D12StateObject *pStateObject)
{
	assert(_interface_version >= 4);

	static_cast<ID3D12GraphicsCommandList4 *>(_orig)->SetPipelineState1(pStateObject);
	wcs::observe(_wwmi_compute, [&](auto &v){v.pipeline(PipelineKind::ray_tracing, reinterpret_cast<uint64_t>(pStateObject));});
	wwmi::native_trace::pipeline_state_bound(this, _orig, pStateObject);

#if RESHADE_ADDON >= 2
	// Currently 'SetPipelineState1' is only used for setting ray tracing pipeline state
	reshade::invoke_addon_event<reshade::addon_event::bind_pipeline>(this, reshade::api::pipeline_stage::all_ray_tracing, to_handle(pStateObject));
#endif
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::DispatchRays(const D3D12_DISPATCH_RAYS_DESC *pDesc)
{
	assert(_interface_version >= 4);

#if RESHADE_ADDON
	assert(pDesc != nullptr);

	if (reshade::invoke_addon_event<reshade::addon_event::dispatch_rays>(
			this,
			reshade::api::resource {},
			pDesc->RayGenerationShaderRecord.StartAddress,
			pDesc->RayGenerationShaderRecord.SizeInBytes,
			reshade::api::resource {},
			pDesc->MissShaderTable.StartAddress,
			pDesc->MissShaderTable.SizeInBytes,
			pDesc->MissShaderTable.StrideInBytes,
			reshade::api::resource {},
			pDesc->HitGroupTable.StartAddress,
			pDesc->HitGroupTable.SizeInBytes,
			pDesc->HitGroupTable.StrideInBytes,
			reshade::api::resource {},
			pDesc->CallableShaderTable.StartAddress,
			pDesc->CallableShaderTable.SizeInBytes,
			pDesc->CallableShaderTable.StrideInBytes,
			pDesc->Width, pDesc->Height, pDesc->Depth))
		return;
#endif

	D3D12_DISPATCH_RAYS_DESC wwmi_desc;
	if (const uint64_t table = wwmi::rt_forward::hit_table(this, _orig, pDesc))
	{
		wwmi_desc = *pDesc;
		wwmi_desc.HitGroupTable.StartAddress = table;
		pDesc = &wwmi_desc;
	}
	const auto wwmi_trace = wwmi::native_trace::dispatch_rays_before(this, _orig, pDesc);
	static_cast<ID3D12GraphicsCommandList4 *>(_orig)->DispatchRays(pDesc);
	wwmi::native_trace::call_after(wwmi_trace, "dispatch_rays", this);
}

void STDMETHODCALLTYPE D3D12GraphicsCommandList::RSSetShadingRate(D3D12_SHADING_RATE BaseShadingRate, const D3D12_SHADING_RATE_COMBINER *pCombiners)
{
	assert(_interface_version >= 5);

	static_cast<ID3D12GraphicsCommandList5 *>(_orig)->RSSetShadingRate(BaseShadingRate, pCombiners);
}
void STDMETHODCALLTYPE D3D12GraphicsCommandList::RSSetShadingRateImage(ID3D12Resource *pShadingRateImage)
{
	assert(_interface_version >= 5);

	static_cast<ID3D12GraphicsCommandList5 *>(_orig)->RSSetShadingRateImage(pShadingRateImage);
}

void STDMETHODCALLTYPE D3D12GraphicsCommandList::DispatchMesh(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ)
{
	assert(_interface_version >= 6);

#if RESHADE_ADDON
	if (reshade::invoke_addon_event<reshade::addon_event::dispatch_mesh>(this, ThreadGroupCountX, ThreadGroupCountX, ThreadGroupCountZ))
		return;
#endif

	static_cast<ID3D12GraphicsCommandList6 *>(_orig)->DispatchMesh(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}

void STDMETHODCALLTYPE D3D12GraphicsCommandList::Barrier(UINT32 NumBarrierGroups, const D3D12_BARRIER_GROUP *pBarrierGroups)
{
	assert(_interface_version >= 7);

	wwmi::binding_trace::enhanced_barriers(static_cast<ID3D12GraphicsCommandList7 *>(_orig), _orig, 1, 1012, NumBarrierGroups, pBarrierGroups);
	wwmi::native_buffer::enhanced(native_buffer_recording_id(this),NumBarrierGroups,pBarrierGroups);

#if RESHADE_ADDON >= 2
	if (!reshade::has_addon_event<reshade::addon_event::barrier>())
		return;

	for (UINT32 k = 0; k < NumBarrierGroups; ++k)
	{
		const D3D12_BARRIER_GROUP &group = pBarrierGroups[k];

		temp_mem<reshade::api::resource> resources(group.NumBarriers);
		temp_mem<reshade::api::resource_usage> old_state(group.NumBarriers), new_state(group.NumBarriers);
		for (UINT i = 0; i < group.NumBarriers; ++i)
		{
			switch (group.Type)
			{
			case D3D12_BARRIER_TYPE_GLOBAL:
				resources[i] = { 0 };
				old_state[i] = reshade::d3d12::convert_access_to_usage(group.pGlobalBarriers[i].AccessBefore);
				new_state[i] = reshade::d3d12::convert_access_to_usage(group.pGlobalBarriers[i].AccessAfter);
				break;
			case D3D12_BARRIER_TYPE_TEXTURE:
				resources[i] = to_handle(group.pTextureBarriers[i].pResource);
				old_state[i] = reshade::d3d12::convert_access_to_usage(group.pTextureBarriers[i].AccessBefore);
				new_state[i] = reshade::d3d12::convert_access_to_usage(group.pTextureBarriers[i].AccessAfter);
				break;
			case D3D12_BARRIER_TYPE_BUFFER:
				resources[i] = to_handle(group.pBufferBarriers[i].pResource);
				old_state[i] = reshade::d3d12::convert_access_to_usage(group.pBufferBarriers[i].AccessBefore);
				new_state[i] = reshade::d3d12::convert_access_to_usage(group.pBufferBarriers[i].AccessAfter);
				break;
			}
		}

		reshade::invoke_addon_event<reshade::addon_event::barrier>(this, group.NumBarriers, resources.p, old_state.p, new_state.p);
	}
#endif
}

void   STDMETHODCALLTYPE D3D12GraphicsCommandList::OMSetFrontAndBackStencilRef(UINT FrontStencilRef, UINT BackStencilRef)
{
	assert(_interface_version >= 8);

	static_cast<ID3D12GraphicsCommandList8 *>(_orig)->OMSetFrontAndBackStencilRef(FrontStencilRef, BackStencilRef);

#if RESHADE_ADDON >= 2
	if (!reshade::has_addon_event<reshade::addon_event::bind_pipeline_states>())
		return;

	const reshade::api::dynamic_state states[2] = { reshade::api::dynamic_state::front_stencil_reference_value, reshade::api::dynamic_state::back_stencil_reference_value };
	const uint32_t values[2] = { FrontStencilRef, BackStencilRef };

	reshade::invoke_addon_event<reshade::addon_event::bind_pipeline_states>(this, static_cast<uint32_t>(std::size(states)), states, values);
#endif
}

void   STDMETHODCALLTYPE D3D12GraphicsCommandList::RSSetDepthBias(FLOAT DepthBias, FLOAT DepthBiasClamp, FLOAT SlopeScaledDepthBias)
{
	assert(_interface_version >= 9);

	static_cast<ID3D12GraphicsCommandList9 *>(_orig)->RSSetDepthBias(DepthBias, DepthBiasClamp, SlopeScaledDepthBias);

#if RESHADE_ADDON >= 2
	if (!reshade::has_addon_event<reshade::addon_event::bind_pipeline_states>())
		return;

	const reshade::api::dynamic_state states[3] = { reshade::api::dynamic_state::depth_bias, reshade::api::dynamic_state::depth_bias_clamp, reshade::api::dynamic_state::depth_bias_slope_scaled };
	const uint32_t values[3] = { *reinterpret_cast<const uint32_t *>(&DepthBias), *reinterpret_cast<const uint32_t *>(&DepthBiasClamp), *reinterpret_cast<const uint32_t *>(&SlopeScaledDepthBias) };

	reshade::invoke_addon_event<reshade::addon_event::bind_pipeline_states>(this, static_cast<uint32_t>(std::size(states)), states, values);
#endif
}
void   STDMETHODCALLTYPE D3D12GraphicsCommandList::IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE IBStripCutValue)
{
	assert(_interface_version >= 9);

	wwmi::binding_trace::strip_cut(static_cast<ID3D12GraphicsCommandList9 *>(_orig), _orig, 1, 1013, IBStripCutValue);
}

void   STDMETHODCALLTYPE D3D12GraphicsCommandList::SetProgram(const D3D12_SET_PROGRAM_DESC *pDesc)
{
	assert(_interface_version >= 10);

	static_cast<ID3D12GraphicsCommandList10 *>(_orig)->SetProgram(pDesc);
	wcs::unobserved(_wwmi_compute, false); // Program bindings do not terminate a render pass.
}
void   STDMETHODCALLTYPE D3D12GraphicsCommandList::DispatchGraph(const D3D12_DISPATCH_GRAPH_DESC *pDesc)
{
	assert(_interface_version >= 10);

	static_cast<ID3D12GraphicsCommandList10 *>(_orig)->DispatchGraph(pDesc);
}
