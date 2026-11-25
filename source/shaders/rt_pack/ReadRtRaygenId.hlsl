// Matches the real native global root's p4 and p6 tables. Do not attach a
// different root signature: the original non-table parameters stay bound.
ByteAddressBuffer native_record : register(t0, space1);
RWByteAddressBuffer receipt : register(u0, space1);

[numthreads(1, 1, 1)]
void main()
{
    receipt.Store4(0, native_record.Load4(0));
    receipt.Store4(16, native_record.Load4(16));
    receipt.Store(32, 0x52474944u);
}
