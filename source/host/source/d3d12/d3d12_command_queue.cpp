/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#include "d3d12_device.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_command_queue.hpp"
#include "d3d12_command_queue_downlevel.hpp"
#include "dll_log.hpp"
#include "com_utils.hpp"
#include "addon_manager.hpp"
#include "wwmi_native_trace.hpp"
#include "wwmi_rt_forward.hpp"
#include "wwmi_post_execute.h"
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace {
struct PostExecuteState {
	WWMI_PostExecuteFn_v1 fn = nullptr;
	uint64_t context = 0;
	std::atomic<uint64_t> epoch{ 0 };
	std::atomic<unsigned> entered{ 0 };
	std::mutex lifecycle;
	std::condition_variable idle;
	bool draining = false;
};
PostExecuteState g_post_execute;
thread_local bool t_in_post_execute = false;

bool PostExecuteActive() noexcept
{
	return (g_post_execute.epoch.load(std::memory_order_acquire) & 1u) != 0u;
}
void PostExecuteLeave() noexcept
{
	if (g_post_execute.entered.fetch_sub(1, std::memory_order_acq_rel) == 1 && !PostExecuteActive())
	{
		std::lock_guard<std::mutex> lock(g_post_execute.lifecycle);
		g_post_execute.idle.notify_all();
	}
}
void PostExecuteInvoke(reshade::api::command_queue *api_queue, ID3D12CommandQueue *native_queue,
	const uint64_t *api_lists, uint32_t count) noexcept
{
	if (t_in_post_execute || !count) return;
	const uint64_t observed = g_post_execute.epoch.load(std::memory_order_acquire);
	if ((observed & 1u) == 0u) return;
	g_post_execute.entered.fetch_add(1, std::memory_order_acq_rel);
	if (g_post_execute.epoch.load(std::memory_order_acquire) != observed)
	{
		PostExecuteLeave();
		return;
	}
	t_in_post_execute = true;
	const auto fn = g_post_execute.fn;
	const uint64_t ctx = g_post_execute.context;
	if (fn)
	{
		try { fn(reinterpret_cast<uint64_t>(api_queue), reinterpret_cast<uint64_t>(native_queue), api_lists, count, ctx); }
		catch (...) {}
	}
	t_in_post_execute = false;
	PostExecuteLeave();
}
}

extern "C" int32_t __cdecl WWMI_RegisterPostExecute_v1(WWMI_PostExecuteFn_v1 callback, uint64_t context)
{
	if (t_in_post_execute) return -3;
	std::unique_lock<std::mutex> lock(g_post_execute.lifecycle);
	if (callback == nullptr)
	{
		if (g_post_execute.draining)
		{
			if (g_post_execute.context != context) return -2;
			g_post_execute.idle.wait(lock, [] { return !g_post_execute.draining; });
			return 0;
		}
		if (!PostExecuteActive()) return (g_post_execute.context == 0 || g_post_execute.context == context) ? 0 : -2;
		if (g_post_execute.context != context) return -2;
		g_post_execute.draining = true;
		g_post_execute.epoch.fetch_add(1, std::memory_order_acq_rel);
		g_post_execute.idle.wait(lock, [] { return g_post_execute.entered.load(std::memory_order_acquire) == 0; });
		g_post_execute.fn = nullptr;
		g_post_execute.context = 0;
		g_post_execute.draining = false;
		g_post_execute.idle.notify_all();
		return 0;
	}
	if (!context || g_post_execute.draining) return -2;
	if (PostExecuteActive())
		return (g_post_execute.fn == callback && g_post_execute.context == context) ? 0 : -2;
	g_post_execute.fn = callback;
	g_post_execute.context = context;
	g_post_execute.epoch.fetch_add(1, std::memory_order_release);
	return 0;
}

D3D12CommandQueue::D3D12CommandQueue(D3D12Device *device, ID3D12CommandQueue *original) :
	command_queue_impl(device, original)
{
	assert(_orig != nullptr && _device != nullptr);
	// Explicitly add a reference to the device, to ensure it stays valid for the lifetime of this queue object
	static_cast<D3D12Device *>(_device)->AddRef();

#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::init_command_queue>(this);
#endif
}
D3D12CommandQueue::~D3D12CommandQueue()
{
#if RESHADE_ADDON
	reshade::invoke_addon_event<reshade::addon_event::destroy_command_queue>(this);
#endif

	// Release the device reference below at the end of 'D3D12CommandQueue::Release' rather than here, since the '~command_queue_impl' destructor still has to run with the device alive
}

bool D3D12CommandQueue::check_and_upgrade_interface(REFIID riid)
{
	if (riid == __uuidof(this) ||
		riid == __uuidof(IUnknown) || // This is the IID_IUnknown identity object
		riid == __uuidof(ID3D12Object) ||
		riid == __uuidof(ID3D12DeviceChild) ||
		riid == __uuidof(ID3D12Pageable))
		return true;

	static constexpr IID iid_lookup[] = {
		__uuidof(ID3D12CommandQueue),  // {0EC870A6-5D7E-4C22-8CFC-5BAAE07616ED}
		__uuidof(ID3D12CommandQueue1), // {3A3C3165-0EE7-4B8E-A0AF-6356B4c3BBB9}
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
#if RESHADE_VERBOSE_LOG
			reshade::log::message(reshade::log::level::debug, "Upgrading ID3D12CommandQueue%hu object %p to ID3D12CommandQueue%hu.", _interface_version, this, version);
#endif
			_orig->Release();
			_orig = static_cast<ID3D12CommandQueue *>(new_interface);
			_interface_version = version;
		}

		return true;
	}

	return false;
}

HRESULT STDMETHODCALLTYPE D3D12CommandQueue::QueryInterface(REFIID riid, void **ppvObj)
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

	// Special case for d3d12on7
	if (riid == __uuidof(ID3D12CommandQueueDownlevel)) // {38A8C5EF-7CCB-4E81-914F-A6E9D072C494}
	{
		if (_downlevel == nullptr)
		{
			// Not a 'com_ptr' since D3D12CommandQueueDownlevel will take ownership
			ID3D12CommandQueueDownlevel *downlevel = nullptr;
			if (SUCCEEDED(_orig->QueryInterface(&downlevel)))
				_downlevel = new D3D12CommandQueueDownlevel(this, downlevel);
		}

		if (_downlevel != nullptr)
			return _downlevel->QueryInterface(riid, ppvObj);
		return E_NOINTERFACE;
	}

	return _orig->QueryInterface(riid, ppvObj);
}
ULONG   STDMETHODCALLTYPE D3D12CommandQueue::AddRef()
{
	_orig->AddRef();
	return InterlockedIncrement(&_ref);
}
ULONG   STDMETHODCALLTYPE D3D12CommandQueue::Release()
{
	const ULONG ref = InterlockedDecrement(&_ref);
	if (ref != 0)
	{
		_orig->Release();
		return ref;
	}

	if (_downlevel != nullptr)
	{
		// Release the reference that was added when the downlevel interface was first queried in 'QueryInterface' above
		_downlevel->_orig->Release();
		delete _downlevel;
	}

	const auto orig = _orig;
	const auto device = static_cast<D3D12Device *>(_device);
	const auto interface_version = _interface_version;
#if RESHADE_VERBOSE_LOG
	reshade::log::message(reshade::log::level::debug, "Destroying ID3D12CommandQueue%hu object %p (%p).", interface_version, this, orig);
#endif
	delete this;

	const ULONG ref_orig = orig->Release();
	if (ref_orig != 0) // Verify internal reference count
		reshade::log::message(reshade::log::level::warning, "Reference count for ID3D12CommandQueue%hu object %p (%p) is inconsistent (%lu).", interface_version, this, orig, ref_orig);

	// Release the explicit reference to the device that was added in the 'D3D12CommandQueue' constructor above now that the queue implementation was destroyed and is no longer referencing it
	device->Release();
	return 0;
}

HRESULT STDMETHODCALLTYPE D3D12CommandQueue::GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData)
{
	return _orig->GetPrivateData(guid, pDataSize, pData);
}
HRESULT STDMETHODCALLTYPE D3D12CommandQueue::SetPrivateData(REFGUID guid, UINT DataSize, const void *pData)
{
	return _orig->SetPrivateData(guid, DataSize, pData);
}
HRESULT STDMETHODCALLTYPE D3D12CommandQueue::SetPrivateDataInterface(REFGUID guid, const IUnknown *pData)
{
	return _orig->SetPrivateDataInterface(guid, pData);
}
HRESULT STDMETHODCALLTYPE D3D12CommandQueue::SetName(LPCWSTR Name)
{
	return _orig->SetName(Name);
}

HRESULT STDMETHODCALLTYPE D3D12CommandQueue::GetDevice(REFIID riid, void **ppvDevice)
{
	return static_cast<D3D12Device *>(_device)->QueryInterface(riid, ppvDevice);
}

void    STDMETHODCALLTYPE D3D12CommandQueue::UpdateTileMappings(ID3D12Resource *pResource, UINT NumResourceRegions, const D3D12_TILED_RESOURCE_COORDINATE *pResourceRegionStartCoordinates, const D3D12_TILE_REGION_SIZE *pResourceRegionSizes, ID3D12Heap *pHeap, UINT NumRanges, const D3D12_TILE_RANGE_FLAGS *pRangeFlags, const UINT *pHeapRangeStartOffsets, const UINT *pRangeTileCounts, D3D12_TILE_MAPPING_FLAGS Flags)
{
	_orig->UpdateTileMappings(pResource, NumResourceRegions, pResourceRegionStartCoordinates, pResourceRegionSizes, pHeap, NumRanges, pRangeFlags, pHeapRangeStartOffsets, pRangeTileCounts, Flags);
}
void    STDMETHODCALLTYPE D3D12CommandQueue::CopyTileMappings(ID3D12Resource *pDstResource, const D3D12_TILED_RESOURCE_COORDINATE *pDstRegionStartCoordinate, ID3D12Resource *pSrcResource, const D3D12_TILED_RESOURCE_COORDINATE *pSrcRegionStartCoordinate, const D3D12_TILE_REGION_SIZE *pRegionSize, D3D12_TILE_MAPPING_FLAGS Flags)
{
	_orig->CopyTileMappings(pDstResource, pDstRegionStartCoordinate, pSrcResource, pSrcRegionStartCoordinate, pRegionSize, Flags);
}
void    STDMETHODCALLTYPE D3D12CommandQueue::ExecuteCommandLists(UINT NumCommandLists, ID3D12CommandList *const *ppCommandLists)
{
	// Synchronize access to this command queue while events are invoked and the immediate command list may be accessed
	std::unique_lock<std::recursive_mutex> lock(_mutex);

	temp_mem<ID3D12CommandList *> command_lists(NumCommandLists);
	const bool wwmi_notify = wwmi::rt_forward::enabled();
	const bool post_exec_notify = PostExecuteActive();
	const bool track_lists = wwmi_notify || post_exec_notify;
	temp_mem<reshade::api::command_list *, 32> wwmi_lists(track_lists ? NumCommandLists : 0);
	for (UINT i = 0; i < NumCommandLists; ++i)
	{
		assert(ppCommandLists[i] != nullptr);
		if (track_lists) wwmi_lists[i] = nullptr;

		if (com_ptr<D3D12GraphicsCommandList> command_list_proxy;
			SUCCEEDED(ppCommandLists[i]->QueryInterface(&command_list_proxy)))
		{
#if RESHADE_ADDON
			reshade::invoke_addon_event<reshade::addon_event::execute_command_list>(this, command_list_proxy.get());
#endif

			// Get original command list pointer from proxy object
			command_lists[i] = command_list_proxy->_orig;
			if (track_lists) wwmi_lists[i] = command_list_proxy.get();
		}
		else
		{
			// This can be a compute command list too, which have no proxy object
			command_lists[i] = ppCommandLists[i];
		}
	}

	flush_immediate_command_list();

	lock.unlock();

	const auto wwmi_trace = wwmi::native_trace::execute_before(this, _orig, NumCommandLists, ppCommandLists, command_lists.p);
	_orig->ExecuteCommandLists(NumCommandLists, command_lists.p);
	wwmi::native_trace::call_after(wwmi_trace, "queue_execute", this);
	if (wwmi_notify)
		for (UINT i = 0; i < NumCommandLists; ++i)
			if (wwmi_lists[i]) wwmi::rt_forward::queue_after(this, _orig, wwmi_lists[i]);
	if (post_exec_notify)
	{
		temp_mem<uint64_t, 32> post_lists(NumCommandLists);
		uint32_t post_count = 0;
		for (UINT i = 0; i < NumCommandLists; ++i)
			if (wwmi_lists[i]) post_lists[post_count++] = reinterpret_cast<uint64_t>(wwmi_lists[i]);
		if (post_count)
			PostExecuteInvoke(this, _orig, post_lists.p, post_count);
	}
}
void    STDMETHODCALLTYPE D3D12CommandQueue::SetMarker(UINT Metadata, const void *pData, UINT Size)
{
	_orig->SetMarker(Metadata, pData, Size);
}
void    STDMETHODCALLTYPE D3D12CommandQueue::BeginEvent(UINT Metadata, const void *pData, UINT Size)
{
	_orig->BeginEvent(Metadata, pData, Size);
}
void    STDMETHODCALLTYPE D3D12CommandQueue::EndEvent()
{
	_orig->EndEvent();
}
HRESULT STDMETHODCALLTYPE D3D12CommandQueue::Signal(ID3D12Fence *pFence, UINT64 Value)
{
	const auto wwmi_trace = wwmi::native_trace::queue_sync_before("queue_signal", this, _orig, pFence, Value);
	const HRESULT hr = _orig->Signal(pFence, Value);
	wwmi::native_trace::call_after(wwmi_trace, "queue_signal", this, true, hr);
	return hr;
}
HRESULT STDMETHODCALLTYPE D3D12CommandQueue::Wait(ID3D12Fence *pFence, UINT64 Value)
{
	const auto wwmi_trace = wwmi::native_trace::queue_sync_before("queue_wait", this, _orig, pFence, Value);
	const HRESULT hr = _orig->Wait(pFence, Value);
	wwmi::native_trace::call_after(wwmi_trace, "queue_wait", this, true, hr);
	return hr;
}
HRESULT STDMETHODCALLTYPE D3D12CommandQueue::GetTimestampFrequency(UINT64 *pFrequency)
{
	return _orig->GetTimestampFrequency(pFrequency);
}
HRESULT STDMETHODCALLTYPE D3D12CommandQueue::GetClockCalibration(UINT64 *pGpuTimestamp, UINT64 *pCpuTimestamp)
{
	return _orig->GetClockCalibration(pGpuTimestamp, pCpuTimestamp);
}
D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE D3D12CommandQueue::GetDesc()
{
	return _orig->GetDesc();
}

HRESULT STDMETHODCALLTYPE D3D12CommandQueue::SetProcessPriority(D3D12_COMMAND_QUEUE_PROCESS_PRIORITY Priority)
{
	assert(_interface_version >= 1);

	return static_cast<ID3D12CommandQueue1 *>(_orig)->SetProcessPriority(Priority);
}
HRESULT STDMETHODCALLTYPE D3D12CommandQueue::GetProcessPriority(D3D12_COMMAND_QUEUE_PROCESS_PRIORITY *pOutValue)
{
	assert(_interface_version >= 1);

	return static_cast<ID3D12CommandQueue1 *>(_orig)->GetProcessPriority(pOutValue);
}
HRESULT STDMETHODCALLTYPE D3D12CommandQueue::SetGlobalPriority(D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY Priority)
{
	assert(_interface_version >= 1);

	return static_cast<ID3D12CommandQueue1 *>(_orig)->SetGlobalPriority(Priority);
}
HRESULT STDMETHODCALLTYPE D3D12CommandQueue::GetGlobalPriority(D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY *pOutValue)
{
	assert(_interface_version >= 1);

	return static_cast<ID3D12CommandQueue1 *>(_orig)->GetGlobalPriority(pOutValue);
}
