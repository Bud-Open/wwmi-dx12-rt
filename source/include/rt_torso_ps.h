#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// Owned CPU shader bytes from the actual effective PSO clone. Only the known
// LocalVF torso consumers request these; no GPU shader table or readback.
struct RtHistoryPixelBlob {
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    std::array<uint32_t,8> target_formats{};
    uint32_t target_count=0;
};
bool RtCopyHistoryPixel(uint64_t pso,uint64_t layout,uint64_t vs,uint64_t ps,RtHistoryPixelBlob&);
bool RtWriteHistoryCapture(const wchar_t* path,const void* bytes,uint32_t size);
bool RtPixelIgnoresPrevious(const RtHistoryPixelBlob&);
bool RtKnownUnusedPreviousVarying(uint64_t pso,uint64_t layout,uint64_t vs,uint64_t ps,uint32_t targets);
using RtMaskedPixelReads=std::array<uint64_t,2>;
bool RtReadProvenMaskedSibling(uint64_t pso,uint64_t layout,uint64_t vs,uint64_t ps,RtMaskedPixelReads&);
