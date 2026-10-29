// Motion vectors of the Attila Engine Performance Fix's DLSS add-on, for other mods (version 1).
//
// Look the function up with GetProcAddress(GetModuleHandleA("dinput8.dll"), "fgb_motion_vectors"); if it is missing,
// or returns NULL, make your own vectors. Call it on the game's render thread, with nothing of yours bound that you
// need kept, any time after the 3D scene's depth is complete in a frame (e.g. right after the game's lighting pass).
// The first call in a frame makes the vectors (the add-on's own pass later in the frame reuses them); later calls in
// the same frame return the same ones. Every D3D11 state the call changes on the immediate context is put back.
//
// The texture: DXGI_FORMAT_R16G16_FLOAT, created on the game's own D3D11 device, returned with an AddRef (the caller
// releases it). Each value is the pixel's previous position minus its current position, in pixels of the render size,
// without this frame's jitter. Below native resolution (DLSS Quality etc.) the scene fills the top-left
// render_w x render_h of the texture; the rest is not written.
//
// Per-object or camera only: the player's choice in this add-on's F11 panel decides, for both mods (per_object says
// which). Per-object motion costs a lot of fps in big battles - every animated unit is drawn a second time on the
// game's render thread (about 2 ms per frame on a dense clump with a Ryzen 9 9950X3D; one player's 6-core CPU went
// from 104 to 40 fps) - so it is off by default; with it on, one pass serves both mods. Do not add a per-object pass
// of your own while copy_hook is 1. When copy_hook is 0 (the add-on not installed or not running, or a GPU without
// frame generation), this dll has not patched the game's record copy and your mod may. Check that the call is still
// the game's (E8 to the game's memcpy) before patching; this dll does the same and leaves a changed call alone.
#ifndef FGB_MOTION_API_H
#define FGB_MOTION_API_H
#include <d3d11.h>

#define FGB_MOTION_API_VERSION 1

typedef struct FgbMotionInfo {
    unsigned size;                 // set by the caller: sizeof(FgbMotionInfo); smaller -> the call returns NULL
    unsigned version;              // filled in: FGB_MOTION_API_VERSION of the dll (newer versions only add fields)
    unsigned copy_hook;            // filled in, also when NULL is returned: 1 = this dll has patched the game's instance
                                   // record copy (the memcpy call at Empire.Retail.dll+0x17DABAF); do not patch it too
    unsigned frame;                // the add-on's frame counter: changes every frame, equal = the same vectors
    unsigned per_object;           // 1: moving units carry their own motion; 0: camera motion only (the player's choice)
    unsigned render_w, render_h;   // the scene's size in the texture (top-left corner)
    unsigned tex_w, tex_h;         // the texture's size (the game's back buffer size)
    float jitter_x, jitter_y;      // this frame's sub-pixel offset of the scene, render pixels (x right, y down)
} FgbMotionInfo;

typedef ID3D11Texture2D *(__cdecl *FgbMotionVectorsFn)(FgbMotionInfo *info);

#endif
