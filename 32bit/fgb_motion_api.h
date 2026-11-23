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

// The add-on's state, without making motion vectors: GetProcAddress(..., "fgb_state"). Returns 1 and fills *state
// (size set by the caller, as above); 0 if size is too small. Any thread; the values are this frame's on the render
// thread. With DLSS below native size the 3D scene is drawn into the top-left render_w x render_h of the game's
// screen-size targets from the frame's start until the unit banners (scaled = 1 meanwhile); the UI is full size.
#define FGB_STATE_VERSION 1

typedef struct FgbState {
    unsigned size;                 // set by the caller: sizeof(FgbState)
    unsigned version;              // filled in: FGB_STATE_VERSION of the dll (newer versions only add fields)
    unsigned running;              // 1: the add-on presents the game's frames this session
    unsigned sr_mode;              // 0 off, 1 DLAA, 2 Quality, 3 Balanced, 4 Performance, 5 Ultra Performance
    unsigned render_w, render_h;   // the 3D scene's size (= tex_w x tex_h with DLAA or super resolution off)
    unsigned tex_w, tex_h;         // the game's back buffer size
    unsigned scaled;               // 1: the game is drawing the 3D scene at render_w x render_h right now
    float jitter_x, jitter_y;      // this frame's sub-pixel offset of the scene, render pixels (x right, y down)
    unsigned frame_generation;     // generated frames per rendered frame: 0 off, 1 = 2x, 2 = 3x, 3 = 4x
    unsigned per_object;           // 1: per-object motion on (the F11 choice), 0: camera only
    unsigned frame;                // the add-on's frame counter
} FgbState;

typedef int(__cdecl *FgbStateFn)(FgbState *state);

// D3D11's own function for a slot of the game's immediate context vtable (index into ID3D11DeviceContextVtbl):
// GetProcAddress(..., "fgb_d3d11_original"). For the slots the add-on hooks it is what the add-on's hook calls, so a
// mod that hooks that function inside d3d11.dll sees the game's calls whenever it loads. NULL until the game's context
// is known, or if the slot holds a function from another module. Calls the add-on makes itself (its motion pass,
// depth write-back, F11 panel) also go through d3d11's functions.
typedef void *(__cdecl *FgbD3D11OriginalFn)(unsigned slot);

#endif
