// The known native skin8_extra kernel adds float6[3..5] to N, normalizes,
// projects T off N, then skins both. Use that operation on AUTHOR vectors.
ByteAddressBuffer RestVectors : register(t0);
ByteAddressBuffer Blends : register(t1);
StructuredBuffer<float4> CanonicalRows : register(t2);
ByteAddressBuffer AuthorShapeDelta : register(t3);
RWStructuredBuffer<float4> SkinnedVectors : register(u0);
cbuffer SkinRange : register(b0) {
    uint VertexCount;uint FirstVertex;uint OutputFirstVertex;float PositionScale;
    uint DeltaFirstVertex;uint DeltaByteBias;
};
uint U16(uint4 w,uint s){return (w[s>>1]>>((s&1)*16))&65535;}
float S8(uint w,uint shift){int v=int(w<<(24-shift))>>24;return max(-1.0,float(v)/127.0);}
float4 Decode(uint w){return float4(S8(w,0),S8(w,8),S8(w,16),S8(w,24));}
float3 Unit(float3 v){float n=dot(v,v);return n>0?v*rsqrt(n):0;}
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){
    if(id.x>=VertexCount)return;uint vertex=FirstVertex+id.x;
    uint4 ids=Blends.Load4(vertex*32),weights=Blends.Load4(vertex*32+16);
    precise float4 r0=0,r1=0,r2=0;
    [unroll]for(uint i=0;i<8;++i){uint row=U16(ids,i)*3;float w=float(U16(weights,i))/65535.0;
        r0+=CanonicalRows[row]*w;r1+=CanonicalRows[row+1]*w;r2+=CanonicalRows[row+2]*w;
    }
    float4 t=Decode(RestVectors.Load(vertex*8)),n=Decode(RestVectors.Load(vertex*8+4));
    uint deltaByte=DeltaByteBias+(vertex-DeltaFirstVertex)*24+12;
    float3 normal=Unit(n.xyz+asfloat(AuthorShapeDelta.Load3(deltaByte)));
    float3 tangent=Unit(t.xyz-normal*dot(t.xyz,normal));
    float sign=PositionScale<0?-1.0:1.0;
    float3 st=Unit(float3(dot(r0.xyz,tangent),dot(r1.xyz,tangent),dot(r2.xyz,tangent)))*sign;
    float3 sn=Unit(float3(dot(r0.xyz,normal),dot(r1.xyz,normal),dot(r2.xyz,normal)))*sign;
    uint target=(OutputFirstVertex+id.x)*2;
    SkinnedVectors[target]=float4(st,t.w);
    SkinnedVectors[target+1]=float4(sn,n.w*sign);
}
