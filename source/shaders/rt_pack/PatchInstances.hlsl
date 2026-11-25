// The caller supplies a BLAS mapping already tied to the identified mesh.
// Preserve the native 64-byte ARRAY layout and all non-AS instance fields.
ByteAddressBuffer OriginalInstances : register(t0);
RWByteAddressBuffer PatchedInstances : register(u0);
cbuffer PatchParameters : register(b0)
{
    uint InstanceCount;
    uint NativeBlasLow;
    uint NativeBlasHigh;
    uint ModBlasLow;
    uint ModBlasHigh;
};
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= InstanceCount) return;
    uint offset = id.x * 64;
    uint4 row0 = OriginalInstances.Load4(offset);
    uint4 row1 = OriginalInstances.Load4(offset + 16);
    uint4 row2 = OriginalInstances.Load4(offset + 32);
    uint4 tail = OriginalInstances.Load4(offset + 48);
    if (tail.z == NativeBlasLow && tail.w == NativeBlasHigh)
        tail.zw = uint2(ModBlasLow, ModBlasHigh);
    PatchedInstances.Store4(offset, row0);
    PatchedInstances.Store4(offset + 16, row1);
    PatchedInstances.Store4(offset + 32, row2);
    PatchedInstances.Store4(offset + 48, tail);
}
