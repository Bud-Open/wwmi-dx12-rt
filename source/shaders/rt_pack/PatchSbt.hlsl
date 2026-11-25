// The observed Lumen local root: t0/space2, t1/space2, four b0/space2 words.
// Only a known 128-byte hit-record layout is supported here.
struct Rule {uint2 oldIB;uint2 oldVB;uint oldOffset;uint oldStride;uint2 newIB;uint2 newVB;uint newOffset;uint newStride;};
ByteAddressBuffer Source : register(t0);
StructuredBuffer<Rule> Rules : register(t1);
RWByteAddressBuffer Output : register(u0);
cbuffer Args : register(b0) {uint recordCount;uint ruleCount;uint2 unused;uint4 id0;uint4 id1;};
uint2 addOffset(uint2 base,uint offset){uint lo=base.x+offset;return uint2(lo,base.y+(lo<base.x));}
[numthreads(64,1,1)] void main(uint3 tid:SV_DispatchThreadID) {
    if(tid.x>=recordCount)return;
    uint offset=tid.x*128;
    for(uint k=0;k<32;k+=4)Output.Store4(offset+k*4,Source.Load4(offset+k*4));
    if(!all(Source.Load4(offset)==id0)||!all(Source.Load4(offset+16)==id1))return;
    uint2 ib=Source.Load2(offset+32),vb=Source.Load2(offset+40);
    uint packed=Source.Load(offset+48),base=Source.Load(offset+52);
    for(uint i=0;i<ruleCount;++i){Rule r=Rules[i];
        if(!all(addOffset(ib,base)==addOffset(r.oldIB,r.oldOffset))||!all(vb==r.oldVB)||(packed&65535)!=r.oldStride)continue;
        Output.Store2(offset+32,r.newIB);Output.Store2(offset+40,r.newVB);
        Output.Store(offset+48,(packed&0xffff0000)|r.newStride);Output.Store(offset+52,r.newOffset);break;
    }
}
