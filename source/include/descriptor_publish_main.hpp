#pragma once
#include <d3d12.h>
#include <cstdint>

// The native receipt pair is required for this main; no second shadow writer
// is installed through the pre-CopyDescriptors SDK event.
bool InstallDescriptorPublisher(HMODULE host);
void StopDescriptorPublisher();
bool StampTextureDescriptor(ID3D12Device* device,uint64_t heap,uint32_t slot,
    ID3D12Resource* original,D3D12_CPU_DESCRIPTOR_HANDLE replacement,ID3D12Resource* replacement_resource);
