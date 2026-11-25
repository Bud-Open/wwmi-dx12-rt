#pragma once

struct ID3D12GraphicsCommandList;
bool RtPortMaskedTrialActive(ID3D12GraphicsCommandList*) noexcept;

namespace MigotoPort {
struct WrapResource;
bool IsWrappedBackBuffer(const WrapResource*) noexcept;
// True for existing callers, or for a proved private destination in the
// reversible preparation of the first strictly recognized masked pass.
bool PermitMaskedTrialWrite(ID3D12GraphicsCommandList*,const WrapResource*) noexcept;
}
