#ifndef _DWDISPLAY_H
#define _DWDISPLAY_H

// dwDisplay — DroidWorks display/backbuffer manager: display bring-up, the
// dirty-rect repaint list, the wrapped screen image, and frame present.
//
// Ghidra (DroidWorks.exe):
//   dwDisplay_InitDirtyList      dwDisplay_FreeDirtyList
//   dwDisplay_Open               dwDisplay_CreateCursorSprites
//   dwDisplay_Shutdown           dwDisplay_FreeCursors
//   dwDisplay_SetMode            dwDisplay_AddDirtyRect
//   dwDisplay_Present            dwDisplay_InsertDirtyNode
//   (the dwRect_Overlaps/Clip/Union helpers live in dwRect.c)
//
// Buffer model (binary -> OpenJKDF2 mapping, see dwDisplay.cpp):
//   back/working buffer (unnamed, the whole DW GUI draws here)
//       -> dwDisplay_pBackVBuf  = &Video_otherBuf
//   front/primary buffer (unnamed, what the player sees)
//       -> dwDisplay_pFrontVBuf = &Video_menuBuffer (pushed to the window via
//          stdDisplay_DDrawGdiSurfaceFlip after each present)
//
// Compiled as C++ (dwDisplay_SetMode carries an MSVC EH frame and constructs
// a dwImageVBuf with operator new); the whole API keeps C linkage via these
// guards.

#include "Dw/dwTypes.h"
#include "Dw/dwRect.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dwImageVBuf dwImageVBuf; // C++ class (Dw/dwImageVBuf.h); opaque in the C view

// Dirty-rect list node (binary /DW struct dwDirtyRect, size 0x10). The list
// is circular with a rect-less sentinel (dwDisplay_pDirtyList); the four
// shorts at +0x08 have the same LTRB layout as dwRect.
typedef struct dwDirtyRect dwDirtyRect;
struct dwDirtyRect
{
    dwDirtyRect* pNext; // 0x00
    dwDirtyRect* pPrev; // 0x04
    int16_t left;       // 0x08
    int16_t top;        // 0x0a
    int16_t right;      // 0x0c
    int16_t bottom;     // 0x0e
};

// Display environment/device selection (binary: stdBuildDisplayEnvironment
// result + FindClosestDevice index + the hardware-3D caps scan results).
extern void* dwDisplay_pEnv;          // binary (always NULL here — see dwDisplay_Open)
extern int dwDisplay_deviceIdx;
extern char dwDisplay_bHardware3D;    // binary (forced 0 — HW path stubbed)
extern uint32_t dwDisplay_hw3DDevice;
extern uint32_t dwDisplay_hw3DMode;

// The screen wrapped as a dwImage (rebuilt by dwDisplay_SetMode; wraps the
// BACK buffer — everything the DW GUI blits through it lands offscreen until
// dwDisplay_Present).
extern dwImageVBuf* dwDisplay_pScreenImage;

// Screen-sized 16bpp system-memory scratch buffer created on the software
// path (binary: dwDisplay_pSwBuffer; the mode format with bpp forced to 16).
extern tVBuffer* dwDisplay_pSwBuffer;

// Sentinel of the circular dirty-rect list (binary: dwDisplay_pDirtyList; consumed by
// dwGuiScreen_RenderActive/SegUpdate to decide what to repaint).
extern dwDirtyRect* dwDisplay_pDirtyList;

// Back/front buffers (see the buffer model above). NULL until
// dwDisplay_SetMode succeeds.
extern tVBuffer* dwDisplay_pBackVBuf;
extern tVBuffer* dwDisplay_pFrontVBuf;

// Added: hardware (OpenGL, std3D) rendering of the DW 3D views.
//   dwDisplay_bHwRender — the dw_hwRender cvar link (1 = the mission view renders
//     through std3D into the GL scene FBO; 0 = the original software rasterizer,
//     the binary's SW path). Default 1.
//   dwDisplay_bHwScene  — nonzero while a GL-rendered scene is live in std3D's
//     scene FBO (set by dwGuiInGame::Draw, cleared on Deactivate). The window
//     present then composites the scene UNDER the 8-bit GUI (std3D_DrawSceneFbo +
//     the letterboxed menu quad) instead of clearing it: Window_SdlUpdate skips
//     std3D_StartScene/EndScene, std3D_DrawSceneFbo presents, and the std3D world
//     pass maps the 640x480 DW canvas space onto the FBO's 4:3 letterbox.
extern int dwDisplay_bHwRender;
extern int dwDisplay_bHwScene;
//   dwDisplay_bHwView   — nonzero inside a workshop/preview view's GL render bracket
//     (dwGui3DView_BeginHwRender..EndHwRender): std3D maps the 640x480 GUI space the
//     same way and lights with rdColormap_pCurMap; the view is then composited into
//     std3D's persistent view layer, presented under the 8-bit GUI wherever the GUI
//     still holds the view's own quantized pixels (see dwGui3DView.cpp).
extern int dwDisplay_bHwView;

// Added: hardware-scene blend mask. With dw_hwRender the mission view's viewport is a
// hole (palette index 0) in the 8-bit GUI and the GL scene shows through it. GUI code
// that blends through the colormap's transparency table (the item bar's dimmed icons and
// translucent triangle, anti-aliased font edges) would otherwise blend with index 0 and
// come out as opaque garbage (purple icons). While a GL scene is live, a blend onto a
// hole pixel writes the SOURCE index and records it here; the present (menu_f.glsl,
// blendTex/useBlend) draws a GUI pixel that still equals its mask entry at half alpha
// over the scene — the software path's 50/50 table blend, done in GL. A later opaque
// write of a different index cancels the entry by itself; dwGuiInGame::Draw clears the
// mask each frame when it cuts the hole. Tightly packed w*h, mirroring the back buffer.
extern uint8_t* dwDisplay_pBlendMask;
extern const uint8_t* dwDisplay_pBlendMaskBase; // the back buffer's pixel base
extern int dwDisplay_blendMaskW;
extern int dwDisplay_blendMaskH;
extern int dwDisplay_blendMaskPitch;
extern int dwDisplay_blendMaskDirty;
void dwDisplay_BlendMaskClear(void);
void dwDisplay_BlendMaskClearRect(const dwRect* pRect);
void dwDisplay_BlendMaskFree(void);

// Store a colormap-blend result: `blended` (= table[*pDst*256+src] or the reverse order)
// normally; the raw `src` plus a mask entry when the pixel is a scene hole.
static inline void dwDisplay_BlendWrite(uint8_t* pDst, uint8_t src, uint8_t blended)
{
    if (dwDisplay_bHwScene && dwDisplay_pBlendMask)
    {
        ptrdiff_t off = pDst - dwDisplay_pBlendMaskBase;
        if (off >= 0 && off < (ptrdiff_t)dwDisplay_blendMaskPitch * dwDisplay_blendMaskH)
        {
            int y = (int)(off / dwDisplay_blendMaskPitch);
            int x = (int)(off - (ptrdiff_t)y * dwDisplay_blendMaskPitch);
            if (x < dwDisplay_blendMaskW)
            {
                uint8_t* pMask = dwDisplay_pBlendMask + (size_t)y * dwDisplay_blendMaskW + x;
                if (*pDst == 0)
                {
                    *pDst = src;
                    *pMask = src;
                    dwDisplay_blendMaskDirty = 1;
                    return;
                }
                if (*pMask != 0)
                {
                    *pMask = 0;
                    dwDisplay_blendMaskDirty = 1;
                }
            }
        }
    }
    *pDst = blended;
}

// Note: no binary counterpart — resets the module globals for OpenJKDF2's
// soft-reset loop and (re)creates the dirty list, which the binary built in a
// CRT static ctor (dwDisplay_StaticInit) and freed atexit.
void dwDisplay_Startup();

// Allocates the dirty list's sentinel node.
void dwDisplay_InitDirtyList();

// Frees every node INCLUDING the sentinel (dwDisplay_pDirtyList is left
// non-NULL/dangling by the binary; call dwDisplay_InitDirtyList to reuse).
void dwDisplay_FreeDirtyList();

// Full display bring-up: device selection, gamma/palette-effects hookup,
// stdDisplay open, colormap load, mode set (software 640x480x8 path; the
// hardware-3D 640x480x16 path is stubbed), cursor sprite creation.
// Returns 1 on success, 0 on failure OR when the display was already up.
int dwDisplay_Open(char* pColormapName);

// (Re)creates the 32x32 cursor sprite + save-under VBuffers in the current
// mode's format (reloading workshop2.cmp first when running 16bpp) and
// selects the arrow cursor.
void dwDisplay_CreateCursorSprites();

// Frees the DW-owned display objects (screen image, scratch buffer, cursor
// sprites/images) and re-opens the sith render defaults. Does NOT tear down
// the engine's display (see the Note in dwDisplay.cpp).
void dwDisplay_Shutdown();

// Frees the cursor sprite/save-under VBuffers and every loaded cursor image.
void dwDisplay_FreeCursors();

// Picks the closest video mode to width x height x bpp and sets it, clears
// the back buffer, presents once, and rewraps the back buffer in a fresh
// dwDisplay_pScreenImage. Returns nonzero on success.
// (The binary call sites push a 4th always-1 argument that the function
// never reads; it is omitted here.)
int dwDisplay_SetMode(int width, int height, int bpp);

// Unions pRect into the first overlapping node of the dirty list (touching
// rects count as overlapping — see dwRect_Overlaps), else appends a new node
// at the tail.
void dwDisplay_AddDirtyRect(dwRect* pRect);

// Draws the software cursor, copies the back buffer to the front, restores
// the cursor save-under, and empties the dirty list.
void dwDisplay_Present(void);

#ifdef __cplusplus
}
#endif

#endif // _DWDISPLAY_H
