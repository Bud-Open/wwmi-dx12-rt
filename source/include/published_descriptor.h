#pragma once
#include "hooks/descriptor_shadow.h"
#include "rt_descriptor_content.h"

bool ReadPublishedDescriptor(const HeapKey&,ResInfo&) noexcept;
bool ReadPublishedDescriptor(const FlatHeapMap::Entry*,uint32_t,ResInfo&) noexcept;
void NotePublishedDescriptorMiss(const HeapKey&,uint32_t frame) noexcept;
bool ReadPublishedBuffer(const HeapKey&,WWMI_RtDescriptorBuffer_v2&) noexcept;
bool ReadPublishedLegacyBuffer(const HeapKey&,WWMI_RtDescriptorSource_v1&) noexcept;
DXGI_FORMAT PublishedViewFormat(const HeapKey&) noexcept;
// An unread INI ->Format query can use a recorded 2D view description
// without borrowing or binding the resource behind that descriptor.
DXGI_FORMAT PublishedUnreadTextureFormat(const HeapKey&) noexcept;
struct PublishedDescriptorDiagnostic {uint64_t origin=0,write=0;bool origin_live=false;};
PublishedDescriptorDiagnostic ReadPublishedDescriptorDiagnostic(const HeapKey&) noexcept;
bool MaterializePublishedDescriptor(ID3D12Device*,const HeapKey&,D3D12_CPU_DESCRIPTOR_HANDLE,ResInfo&) noexcept;
