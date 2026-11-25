#pragma once
#include <stdint.h>

// Synchronous view of the actual native CreateStateObject/AddToStateObject
// input. Only within the successful object's init_pipeline callback. No
// borrowed pointer may be used after that callback, including pSubobjects,
// association pointers, names, bytecode or the AddTo base.
typedef struct WWMI_RtNativeStateObject_v1 {
    uint32_t size, version;
    uint64_t state, device, desc, add_to_base;
} WWMI_RtNativeStateObject_v1;

// 0 only for the same actual object/device on this thread and callback.
// No value on an SDK-created replacement or unobserved native fallback.
typedef uint32_t (*WWMI_RtCurrentNativeStateObjectFn_v1)(
    uint64_t state, WWMI_RtNativeStateObject_v1 *, uint32_t size);
