// Run immediately before a real original TOP_LEVEL ARRAY Build reads this
// same source in NON_PIXEL_SHADER_RESOURCE. Search key is diagnostic only.
ByteAddressBuffer Instances : register(t0);
RWByteAddressBuffer Result : register(u0);
cbuffer Probe : register(b0) {
    uint Rows, TargetCount, Capacity, BuildOrderLow;
    uint BuildOrderHigh;
    uint4 Targets[2];
};
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 lane : SV_GroupThreadID)
{
    if (id.x >= Rows) return;
    if (lane.x == 0) {
        uint ignored;
        Result.InterlockedAdd(4, min(64u, Rows-id.x), ignored);
    }
    if (id.x == 0) Result.Store2(8, uint2(BuildOrderLow, BuildOrderHigh));
    const uint src = id.x * 64;
    const uint4 tail = Instances.Load4(src+48);
    if (!(tail.x & 0x01000000u)) return;
    const bool match =
        (TargetCount > 0 && all(tail.zw == Targets[0].xy)) ||
        (TargetCount > 1 && all(tail.zw == Targets[0].zw)) ||
        (TargetCount > 2 && all(tail.zw == Targets[1].xy)) ||
        (TargetCount > 3 && all(tail.zw == Targets[1].zw));
    if (!match) return;
    uint slot;
    Result.InterlockedAdd(0, 1, slot);
    if (slot >= Capacity) return;
    const uint dst = 16 + slot*80;
    Result.Store(dst, id.x);
    Result.Store4(dst+16, Instances.Load4(src));
    Result.Store4(dst+32, Instances.Load4(src+16));
    Result.Store4(dst+48, Instances.Load4(src+32));
    Result.Store4(dst+64, tail);
}
