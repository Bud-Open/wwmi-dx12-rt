#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>

namespace reshade::api { struct command_list; }
struct RfxTexLatch;
struct ShadowSlotOvr;
struct RtRfxDrawFacts;

struct RtRfxTextureSource {
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
};
using RtRfxTextureSources=std::array<RtRfxTextureSource,4>;

RtRfxTextureSource RtResolveRfxTexture(const std::wstring&, const wchar_t* ns);
void RtPublishPendingHybridNamedTextures();
void RtRememberHybridRfxTextures(RtRfxTextureSources, unsigned named, unsigned resolved);

// Shared with the recorded-table path: plan only, never mutate a game
// descriptor or bind an extra heap during the INI VM.
bool RtFreezeRabbitFxSlots(reshade::api::command_list*, int mod, int component, const RfxTexLatch&,
    const RtRfxTextureSources&, bool gbuffer,
    std::vector<ShadowSlotOvr>&, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>&,
    std::vector<uint64_t>& ps_owners, RtRfxDrawFacts&);
