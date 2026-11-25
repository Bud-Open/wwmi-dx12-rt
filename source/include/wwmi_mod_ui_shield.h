#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WWMI_ModUiRect_v1 {
    float x0; // normalized client X left   [0..1]
    float y0; // normalized client Y top    [0..1]
    float x1; // normalized client X right  [0..1]
    float y1; // normalized client Y bottom [0..1]
} WWMI_ModUiRect_v1;

// Publish currently visible interactive 2D Mod UI rectangles for `hwnd`.
// Pass count == 0 (or rects == nullptr) to clear the click shield.
typedef void (__cdecl *WWMI_ModUiSetShieldRectsFn_v1)(
    void *hwnd,
    const WWMI_ModUiRect_v1 *rects,
    uint32_t count);

// Thread-local bypass around 3Dmigoto's own input polling (`DispatchInputEvents`)
// so 3Dmigoto's GetAsyncKeyState / GetKeyState checks still observe real mouse buttons
// while game code sees shielded clicks suppressed.
typedef void (__cdecl *WWMI_ModUiSetInputBypassFn_v1)(uint32_t enable);

#ifdef __cplusplus
}
#endif
