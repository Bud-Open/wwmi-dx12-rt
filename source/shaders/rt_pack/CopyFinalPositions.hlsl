// Input is already skinned and fully deformed in object-local coordinates.
StructuredBuffer<float3> FinalPositions : register(t0);
RWStructuredBuffer<float3> Positions : register(u0);
cbuffer CopyRange : register(b0) {uint Count;uint FirstInput;uint FirstOutput;};
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID){if(id.x<Count)Positions[FirstOutput+id.x]=FinalPositions[FirstInput+id.x];}
