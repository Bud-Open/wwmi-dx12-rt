// Post-multiplier author-topology float6: xyz displacement, NOT final position.
// The producer seals an immutable snapshot; neither native indices nor native
// vertex offsets address it. The blend/palette contract is SkinPositions.hlsl's.
StructuredBuffer<float3> RestPositions : register(t0);
ByteAddressBuffer Blends : register(t1);
StructuredBuffer<float4> CanonicalRows : register(t2);
ByteAddressBuffer AuthorShapeDelta : register(t3);
RWStructuredBuffer<float3> SkinnedPositions : register(u0);

cbuffer SkinRange : register(b0)
{
    uint VertexCount;
    uint FirstVertex;
    uint OutputFirstVertex;
    float PositionScale;
    uint DeltaFirstVertex;
    uint DeltaByteBias;
};

uint UnpackU16(uint4 words, uint slot)
{
    return (words[slot >> 1] >> ((slot & 1) * 16)) & 0xffff;
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= VertexCount)
        return;
    uint vertex = FirstVertex + tid.x;
    uint deltaByte = DeltaByteBias + (vertex - DeltaFirstVertex) * 24;
    precise float3 deformed = RestPositions[vertex] + asfloat(AuthorShapeDelta.Load3(deltaByte));

    uint4 ids = Blends.Load4(vertex * 32);
    uint4 weights = Blends.Load4(vertex * 32 + 16);
    precise float4 r0 = 0;
    precise float4 r1 = 0;
    precise float4 r2 = 0;
    [unroll]
    for (uint i = 0; i < 8; ++i)
    {
        uint row = UnpackU16(ids, i) * 3;
        float weight = (float)UnpackU16(weights, i) / 65535.0;
        r0 += CanonicalRows[row] * weight;
        r1 += CanonicalRows[row + 1] * weight;
        r2 += CanonicalRows[row + 2] * weight;
    }
    float4 p = float4(deformed, 1.0);
    precise float3 result = float3(dot(r0, p), dot(r1, p), dot(r2, p));
    SkinnedPositions[OutputFirstVertex + tid.x] = result * PositionScale;
}
