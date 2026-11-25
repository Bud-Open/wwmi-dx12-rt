ByteAddressBuffer source : register(t0);
RWByteAddressBuffer destination : register(u0);
cbuffer CopyDomain : register(b0) { uint word_count; uint source_byte_offset; };
[numthreads(64,1,1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint word = tid.x + tid.y * (65535 * 64);
    if (word < word_count) destination.Store(4 * word, source.Load(source_byte_offset + 4 * word));
}
