// dwGui3DView — the DroidWorks base 3D-view widget (embedded rdCanvas +
// rdCamera + two rdLights rendering into the DW back buffer).
//
// Decompiled from DroidWorks.exe.
// dwGui3DView_vtbl. See dwGui3DView.h for the field/slot map.
//
// Engine mapping notes:
//  - The binary's `rdCanvas_New` label at the ctor call site is the engine's
//    rdCanvas_NewEntry (in-place init of the EMBEDDED canvas); the repo's
//    9th parameter (a9 -> field_14) is not visible in the 8-arg binary call,
//    so 0 is passed.
//  - rd_FUN_0047eb60(2) resolves to rdSetZBufferMethod (the DW rdroid setter
//    cluster mirrors src/Engine/rdroid.c's function order).
//  - The dtor's `sithRender_Open()` call is a mislabel: the target is an empty
//    ICF-folded COMDAT == rdCanvas_FreeEntry (a no-op in the engine too).
//  - rdMatrix_FUN_00486af0 == rdMatrix_TransformPoint34Acc (verified from its
//    body: in-place point transform incl. translation).
//  - DAT_006b5a40 (the canvas target tVBuffer) is the DW back buffer ==
//    dwDisplay_pBackVBuf; the d3d/scratch buffer slot gets dwDisplay_pSwBuffer.
//
// No module statics — no dwGui3DView_Startup needed (soft-reset rule).

#include "Dw/dwGui3DView.h"

#include "Dw/dwDisplay.h"
#include "Dw/dwImageDraw.h"

#include "jk.h"
#include "Engine/rdCamera.h"
#include "Engine/rdCanvas.h"
#include "Primitives/rdMatrix.h"
// This engine header has no extern "C" guards of its own — wrap at include site.
extern "C" {
#include "Engine/rdroid.h"
#include "Win95/stdDisplay.h"
#ifdef RDRASTER_SW_ZBUFFER
#include "Raster/rdZRaster.h" // SW z-buffer per-frame clear (shared bracket)
#include "Platform/std3D.h"   // Added: std3D_DwReadbackRect (GL bracket)
#include <stdio.h>             // Added: DW_READBACK_DUMP
#include <stdlib.h>            // Added: getenv
#include <string.h>            // Added: memset
#include "stdPlatform.h"
#endif
}

// ---- ctor/dtor -----------------------------------------------------------------

// (Ghidra: dwGui3DView_Ctor)
dwGui3DView::dwGui3DView(dwRect* pRect)
    : dwWidget(pRect)
{
    rdSetRenderOptions(rdGetRenterOptions() | 3);
    rdSetZBufferMethod(RD_ZBUFFER_READ_WRITE); // binary: rd_FUN_0047eb60(2)

    // In-place canvas over the DW back buffer, covering the widget rect
    // (binary: rdCanvas_NewEntry-equivalent call; a9/field_14 not present in
    // the 8-arg binary call — pass 0).
    rdCanvas_NewEntry(&this->canvas, 3, dwDisplay_pBackVBuf, dwDisplay_pSwBuffer,
                      pRect->left, pRect->top, pRect->right - 1, pRect->bottom - 1, 0);
    this->canvas.half_screen_width = (flex_t)(pRect->left + pRect->right) * 0.5;
    this->canvas.half_screen_height = (flex_t)(pRect->top + pRect->bottom) * 0.5;

    // Binary zeroes exactly the embedded camera before rdCamera_NewEntry
    // (0x119 dwords @+0x68).
    _memset(&this->camera, 0, sizeof(this->camera));
    rdCamera_NewEntry(&this->camera, 20.0, 0, 0.008, 50.0, 1.0);
    rdCamera_SetCanvas(&this->camera, &this->canvas);
    rdCamera_SetAmbientLight(&this->camera, 0.3);
    rdCamera_SetAttenuation(&this->camera, 1e-5, 1e-5);

    // Note: the binary leaves the two rdLights (and position vectors)
    // uninitialized heap garbage until SetLight0/1 fill pos/intensity/type
    // (id/radii are filled by rdCamera_AddLight). Zeroing here keeps the
    // untouched fields (bEnabled/direction/color) deterministic on 64-bit.
    _memset(&this->light0Pos, 0, sizeof(this->light0Pos));
    _memset(&this->light1Pos, 0, sizeof(this->light1Pos));
    _memset(&this->light0, 0, sizeof(this->light0));
    _memset(&this->light1, 0, sizeof(this->light1));
    // Note: viewMat is NOT initialized here (faithful) — dwGuiViewBox builds
    // it in its ctor; a bare dwGui3DView must not be drawn before someone
    // sets it.

    this->SetLight0(0.0f, -1.0f, -0.4f, 1.0f, 2);
    this->SetLight1(0.0f, -0.2f, 1.0f, 0.75f, 2);
    dwGui3DView::RebuildLights(); // binary: direct call (vtable is this class's here anyway)
    dwGui3DView::Refresh();       // binary: direct call
}

// (Ghidra: dwGui3DView_Dtor; dwGui3DView_DtorDelete)
dwGui3DView::~dwGui3DView()
{
    rdCanvas_FreeEntry(&this->canvas); // binary: ICF-folded empty fn (mislabeled sithRender_Open)
    rdCamera_FreeEntry(&this->camera);
    // dwWidget base dtor runs automatically.
}

// ---- virtuals ------------------------------------------------------------------

// (Ghidra: dwGui3DView_OnMessage) — vtbl +0x1c.
// Msg 0x3ea = set FOV/zoom; the numeric payload rides in the pSender slot
// (percent: fov = payload * 0.01).
int dwGui3DView::OnMessage(dwWidgetMsg* pMsg)
{
    if (pMsg->code == 0x3ea)
    {
        float newFov = (float)(uint32_t)(uintptr_t)pMsg->pSender * 0.01f;
        if (newFov != this->camera.fov)
        {
            // Re-sync the frustum to the WIDGET rect when the canvas cache
            // is stale (the canvas x/y/w/h fields double as a "last frustum
            // rect" cache in this class).
            rdCanvas* pCanvas = this->camera.pCanvas;
            if (pCanvas->xStart != this->left || pCanvas->yStart != this->top
                || pCanvas->widthMinusOne != this->right || pCanvas->heightMinusOne != this->bottom)
            {
                rdCamera_SetFrustrum(&this->camera, this->camera.pClipFrustum,
                                     this->left, this->top, this->right, this->bottom);
                pCanvas->xStart = this->left;
                pCanvas->yStart = this->top;
                pCanvas->widthMinusOne = this->right;
                pCanvas->heightMinusOne = this->bottom;
            }
            this->camera.fov = newFov;
            rdCamera_BuildFOV(&this->camera);
            this->Refresh(); // vtbl +0x4c
        }
    }
    return 0;
}

// (Ghidra: dwGui3DView_Move; recovered fn) — vtbl +0x28
void dwGui3DView::Move(int16_t dx, int16_t dy)
{
    this->left = this->left + dx;
    this->top = this->top + dy;
    this->right = this->right + dx;
    this->bottom = this->bottom + dy;
    this->camera.pCanvas->half_screen_width = (flex_t)(this->right + this->left) * 0.5;
    this->camera.pCanvas->half_screen_height = (flex_t)(this->top + this->bottom) * 0.5;
}

// (Ghidra: dwGui3DView_Draw) — vtbl +0x44. Camera bring-up only;
// draws no pixels (derived classes render content after calling this).
// pDestBits is unused (faithful).
void dwGui3DView::Draw(dwImageBits* pDestBits, dwRect* pClipRect)
{
    rdCamera_SetCurrent(&this->camera);
    rdCamera_Update(&this->viewMat);

    rdCanvas* pCanvas = this->camera.pCanvas;
    int x1 = (int16_t)(pClipRect->left + 1);
    int y1 = (int16_t)(pClipRect->top + 1);
    int x2 = pClipRect->right;
    int y2 = pClipRect->bottom;
    if (pCanvas->xStart != x1 || pCanvas->yStart != y1
        || pCanvas->widthMinusOne != x2 || pCanvas->heightMinusOne != y2)
    {
        rdCamera_SetFrustrum(&this->camera, this->camera.pClipFrustum, x1, y1, x2, y2);
        pCanvas->xStart = x1;
        pCanvas->yStart = y1;
        pCanvas->widthMinusOne = x2;
        pCanvas->heightMinusOne = y2;
    }
    rdSetLightingMode(3); // RD_LIGHTMODE_GOURAUD
}

// ---- Added: shared software-render bracket (see the header note) ----------------

rdCanvas* dwGui3DView_BeginSwRender(int* pSavedAccel)
{
    rdCanvas* pSwCanvas = rdCamera_g_pCurCamera->pCanvas;
    *pSavedAccel = rdroid_curAcceleration;
    stdDisplay_VBufferLock(pSwCanvas->pVBuffer);

    if (!(rdGetRenterOptions() & 0x100))
        stdDisplay_VBufferFill(rdCamera_g_pCurCamera->pCanvas->d3d_vbuf, 0, NULL);
    rdAdvanceFrame();
    // ⚠ MUST be after rdAdvanceFrame: rdCache_AdvanceFrame force-sets
    // rdroid_curAcceleration = 1 on SDL2_RENDER builds. Setting SW mode here (as
    // jkGame_Update does after its frame advance) makes rdCache_Flush take the
    // software branch so rdZRaster paints the locked 8bpp canvas.
    rdroid_curAcceleration = 0;
#ifdef RDRASTER_SW_ZBUFFER
    rdZRaster_BeginFrame(pSwCanvas->pVBuffer); // SW depth clear (after the accel flip)
#endif
    return pSwCanvas;
}

void dwGui3DView_EndSwRender(rdCanvas* pSwCanvas, int savedAccel)
{
    rdFinishFrame();
    stdDisplay_VBufferUnlock(pSwCanvas->pVBuffer);
    rdroid_curAcceleration = savedAccel;
}

// ---- Added: hardware (GL) render bracket with readback (see the header note) ------

// RGB555 -> nearest colormap index. Index 0 is never emitted: it is the menu overlay's
// transparent index (the GL scene would show through such pixels in the mission HUD), and
// DW reserves 0..4 for background/markers in the pick buffer anyway.
static const rdColormap* dwGui3DView_pLutMap = NULL;
static uint32_t dwGui3DView_lutMapSum = 0;
static uint8_t dwGui3DView_aQuantLut[32768];

static uint32_t dwGui3DView_ColormapSum(const rdColormap* pMap)
{
    uint32_t sum = 0;
    for (int i = 0; i < 256; i++)
        sum = sum * 31 + pMap->colors[i].r * 65536 + pMap->colors[i].g * 256 + pMap->colors[i].b;
    return sum;
}

static void dwGui3DView_EnsureQuantLut(const rdColormap* pMap)
{
    uint32_t sum = dwGui3DView_ColormapSum(pMap);
    if (pMap == dwGui3DView_pLutMap && sum == dwGui3DView_lutMapSum)
        return;
    for (int c = 0; c < 32768; c++)
    {
        int r = (((c >> 10) & 31) * 255) / 31;
        int g = (((c >> 5) & 31) * 255) / 31;
        int b = ((c & 31) * 255) / 31;
        int best = 1, bestD = 0x7fffffff;
        for (int i = 1; i < 256; i++)
        {
            int dr = r - pMap->colors[i].r, dg = g - pMap->colors[i].g, db = b - pMap->colors[i].b;
            int d = dr * dr + dg * dg + db * db;
            if (d < bestD) { bestD = d; best = i; }
        }
        dwGui3DView_aQuantLut[c] = (uint8_t)best;
    }
    dwGui3DView_pLutMap = pMap;
    dwGui3DView_lutMapSum = sum;
}

// DW_HW_READBACK=1 forces the readback (256-color) compositing for every view — a debug
// switch to compare against the direct layer path.
static int dwGui3DView_bReadback()
{
    static int cached = -1;
    if (cached < 0)
    {
        const char* p = getenv("DW_HW_READBACK");
        cached = (p && *p && *p != '0') ? 1 : 0;
    }
    return cached;
}

rdCanvas* dwGui3DView_BeginHwRender(int* pSavedSw)
{
    rdCanvas* pCanvas = rdCamera_g_pCurCamera->pCanvas;
    *pSavedSw = rdroid_bSoftwareRenderer;
    rdroid_bSoftwareRenderer = 0; // rdCache_Flush takes the std3D path (DW forces SW otherwise)
    dwDisplay_bHwView = 1;        // std3D: DW 640x480 mapping + light with rdColormap_pCurMap
    rdAdvanceFrame();             // std3D_StartScene: clears the scene FBO (color + depth)
    // Direct mode: seed the view's rect from the layer (index-0 GUI pixels inside this rect
    // keep the layer's pixels — std3D_DwCompositeViewRect discards them from the backdrop).
    if (!dwGui3DView_bReadback())
    {
        int x = pCanvas->xStart, y = pCanvas->yStart;
        std3D_DwBeginViewRect(x, y, pCanvas->widthMinusOne - x + 1, pCanvas->heightMinusOne - y + 1);
    }
    return pCanvas;
}

// Readback (256-color) compositing of the rect the view just rendered: box-filter the scene
// FBO to one sample per GUI pixel, quantize to rdColormap_pCurMap, write the covered pixels
// into the 8bpp canvas. Always run (the 8-bit GUI keeps real pixels; see EndHwRender); the
// part-pick render samples its result from the 8-bit screen.
static void dwGui3DView_ReadbackIntoCanvas(rdCanvas* pCanvas)
{

    int x = pCanvas->xStart, y = pCanvas->yStart;
    int w = pCanvas->widthMinusOne - x + 1, h = pCanvas->heightMinusOne - y + 1;
    tVBuffer* pVBuf = pCanvas->pVBuffer;
    const rdColormap* pMap = rdColormap_pCurMap;
    if (pVBuf && pMap && w > 0 && h > 0 && x >= 0 && y >= 0
        && x + w <= (int)pVBuf->format.width && y + h <= (int)pVBuf->format.height)
    {
        uint8_t* pRgb = (uint8_t*)malloc((size_t)w * h * 3);
        uint8_t* pCov = (uint8_t*)malloc((size_t)w * h);
        if (pRgb && pCov && std3D_DwReadbackRect(x, y, w, h, pRgb, pCov))
        {
            // Added: DW_READBACK_DUMP=<file.ppm> writes the raw readback of the LARGEST view
            // rendered so far (debug: separates GL output from the palette quantization).
            {
                static int dumpMaxArea = 0;
                const char* pDump = getenv("DW_READBACK_DUMP");
                if (pDump && w * h > dumpMaxArea)
                {
                    dumpMaxArea = w * h;
                    FILE* f = fopen(pDump, "wb");
                    if (f)
                    {
                        fprintf(f, "P6\n%d %d\n255\n", w, h);
                        fwrite(pRgb, 1, (size_t)w * h * 3, f);
                        fclose(f);
                    }
                    stdPlatform_Printf("DWRB: dumped %dx%d at (%d,%d); colormap %p colors[20]=%d,%d,%d lightlevel=%p\n",
                        w, h, x, y, (void*)pMap, pMap->colors[20].r, pMap->colors[20].g, pMap->colors[20].b, (void*)pMap->lightlevel);
                }
            }
            dwGui3DView_EnsureQuantLut(pMap);
            stdDisplay_VBufferLock(pVBuf);
            uint8_t* pPix = (uint8_t*)pVBuf->surface_lock_alloc;
            if (pPix)
            {
                int stride = pVBuf->format.rowSize;
                for (int gy = 0; gy < h; gy++)
                {
                    uint8_t* pRow = pPix + (size_t)(y + gy) * stride + x;
                    for (int gx = 0; gx < w; gx++)
                    {
                        int o = gy * w + gx;
                        if (!pCov[o]) continue; // nothing drawn: keep the GUI underneath
                        int c = ((pRgb[o * 3 + 0] >> 3) << 10) | ((pRgb[o * 3 + 1] >> 3) << 5) | (pRgb[o * 3 + 2] >> 3);
                        pRow[gx] = dwGui3DView_aQuantLut[c];
                    }
                }
            }
            stdDisplay_VBufferUnlock(pVBuf);
        }
        free(pRgb);
        free(pCov);
    }
}

// Direct compositing: the GUI pixels under the view go behind the droid in std3D's view
// layer (std3D_DwCompositeViewRect). Returns 0 when the layer is unavailable.
static int dwGui3DView_CompositeIntoLayer(rdCanvas* pCanvas)
{
    int x = pCanvas->xStart, y = pCanvas->yStart;
    int w = pCanvas->widthMinusOne - x + 1, h = pCanvas->heightMinusOne - y + 1;
    tVBuffer* pVBuf = pCanvas->pVBuffer;
    if (!pVBuf || w <= 0 || h <= 0 || x < 0 || y < 0
        || x + w > (int)pVBuf->format.width || y + h > (int)pVBuf->format.height)
        return 0;
    int bDone = 0;
    stdDisplay_VBufferLock(pVBuf);
    uint8_t* pPix = (uint8_t*)pVBuf->surface_lock_alloc;
    if (pPix)
        bDone = std3D_DwCompositeViewRect(x, y, w, h, pPix + (size_t)y * pVBuf->format.rowSize + x, pVBuf->format.rowSize);
    stdDisplay_VBufferUnlock(pVBuf);
    return bDone;
}

// After the readback wrote the view's quantized pixels into the canvas: the same pixels
// become the present's reference mask (see std3D_DwSetViewRef).
static void dwGui3DView_SetViewRef(rdCanvas* pCanvas)
{
    int x = pCanvas->xStart, y = pCanvas->yStart;
    int w = pCanvas->widthMinusOne - x + 1, h = pCanvas->heightMinusOne - y + 1;
    tVBuffer* pVBuf = pCanvas->pVBuffer;
    if (!pVBuf || w <= 0 || h <= 0 || x < 0 || y < 0
        || x + w > (int)pVBuf->format.width || y + h > (int)pVBuf->format.height)
        return;
    stdDisplay_VBufferLock(pVBuf);
    uint8_t* pPix = (uint8_t*)pVBuf->surface_lock_alloc;
    if (pPix)
        std3D_DwSetViewRef(x, y, w, h, pPix + (size_t)y * pVBuf->format.rowSize + x, pVBuf->format.rowSize);
    stdDisplay_VBufferUnlock(pVBuf);
}

// Direct mode: the view goes into the GL view layer (full window resolution and color)
// AND, as in readback mode, quantized into the 8-bit canvas. The present shows the layer
// wherever the GUI pixel still equals that quantized pixel, so the 8-bit draw order is
// untouched: widgets drawn after the view (markers, dialogs, the cursor) and GUI code that
// reads or tints what is under it (dimmed part icons, translucent panels, shaded screen
// snapshots) all see and produce real pixels, exactly as with the software rasterizer.
void dwGui3DView_EndHwRender(rdCanvas* pCanvas, int savedSw)
{
    rdFinishFrame();              // flushes the render list into the scene FBO
    int bLayer = !dwGui3DView_bReadback() && dwGui3DView_CompositeIntoLayer(pCanvas);
    dwGui3DView_ReadbackIntoCanvas(pCanvas);
    if (bLayer)
        dwGui3DView_SetViewRef(pCanvas);
    dwDisplay_bHwView = 0;
    rdroid_bSoftwareRenderer = savedSw;
}

void dwGui3DView_EndHwRenderReadback(rdCanvas* pCanvas, int savedSw)
{
    rdFinishFrame();
    dwGui3DView_ReadbackIntoCanvas(pCanvas);
    dwDisplay_bHwView = 0;
    rdroid_bSoftwareRenderer = savedSw;
}

rdCanvas* dwGui3DView_BeginRender(int* pSaved)
{
    if (dwDisplay_bHwRender)
    {
        int savedSw = 0;
        rdCanvas* pCanvas = dwGui3DView_BeginHwRender(&savedSw);
        *pSaved = (savedSw & 0xffff) | DWGUI3DVIEW_RENDER_HW_BIT;
        return pCanvas;
    }
    int savedAccel = 0;
    rdCanvas* pCanvas = dwGui3DView_BeginSwRender(&savedAccel);
    *pSaved = savedAccel & 0xffff;
    return pCanvas;
}

// The part-pick render always goes through the software rasterizer: the paint tool reads
// the palette index at the click point from the 8-bit screen and that index identifies
// the part's paint region (its contact color), so it must be the exact texel index —
// std3D's output passes through the colormap's RGB, texture filtering and the box
// filter + nearest-color quantization of the readback and does not come back exact.
rdCanvas* dwGui3DView_BeginPickRender(int* pSaved)
{
    int savedAccel = 0;
    rdCanvas* pCanvas = dwGui3DView_BeginSwRender(&savedAccel);
    *pSaved = savedAccel & 0xffff;
    return pCanvas;
}

void dwGui3DView_EndRender(rdCanvas* pCanvas, int saved)
{
    if (saved & DWGUI3DVIEW_RENDER_READBACK_BIT)
        dwGui3DView_EndHwRenderReadback(pCanvas, saved & 0xffff);
    else if (saved & DWGUI3DVIEW_RENDER_HW_BIT)
        dwGui3DView_EndHwRender(pCanvas, saved & 0xffff);
    else
        dwGui3DView_EndSwRender(pCanvas, saved & 0xffff);
}

// (Ghidra: dwGui3DView_RebuildLights) — vtbl +0x48 (NEW virtual).
// Re-adds each light whose |intensity| > 1e-5 (quirk preserved: a NEGATIVE
// intensity beyond the epsilon is added too).
void dwGui3DView::RebuildLights()
{
    rdCamera_ClearLights(&this->camera);

    float intensity0 = this->light0.intensity;
    if (intensity0 < 0.0f)
        intensity0 = -intensity0;
    if (intensity0 <= 1e-5f)
        intensity0 = 0.0f;
    else
        intensity0 = this->light0.intensity;
    if (intensity0 != 0.0f)
        rdCamera_AddLight(&this->camera, &this->light0, &this->light0Pos);

    float intensity1 = this->light1.intensity;
    if (intensity1 < 0.0f)
        intensity1 = -intensity1;
    if (intensity1 <= 1e-5f)
        intensity1 = 0.0f;
    else
        intensity1 = this->light1.intensity;
    if (intensity1 != 0.0f)
        rdCamera_AddLight(&this->camera, &this->light1, &this->light1Pos);
}

// (Ghidra: dwGui3DView_Refresh) — vtbl +0x4c (NEW virtual):
// forwards to virtual Invalidate (+0x34).
void dwGui3DView::Refresh()
{
    this->Invalidate();
}

// ---- non-virtual helpers -------------------------------------------------------

// (Ghidra: dwGui3DView_ScreenToWorld)
void dwGui3DView::ScreenToWorld(dwPoint* pPt, rdVector3* pOut)
{
    pOut->x = (flex_t)pPt->x;
    pOut->z = -(flex_t)pPt->y;
    pOut->y = this->camera.focalLength; // Ghidra: "groundPlaneY"@0xa4 — the projection distance
    pOut->x = pOut->x - this->canvas.half_screen_width;
    pOut->z = this->canvas.half_screen_height + pOut->z;
    rdMatrix_TransformPoint34Acc(pOut, &this->viewMat); // binary: rdMatrix_FUN_00486af0
}

// (Ghidra: dwGui3DView_ProjectPoint)
void dwGui3DView::ProjectPoint(rdVector3* pOut, rdVector3* pWorldPos)
{
    if (rdCamera_g_pCurCamera != &this->camera) // binary: DAT_005542cc
    {
        rdCamera_SetCurrent(&this->camera);
        rdCamera_Update(&this->viewMat);
    }
    rdVector3 world;
    world.x = pWorldPos->x;
    world.y = pWorldPos->y;
    world.z = pWorldPos->z;
    rdVector3 camSpace;
    rdMatrix_TransformPoint34(&camSpace, &world, &this->camera.orient);
    this->camera.pfProject(pOut, &camSpace); // Ghidra: "pProjCallback"@0xb4
}

// (Ghidra: dwGui3DView_DrawMarker)
void dwGui3DView::DrawMarker(dwImageBits* pDestBits, rdVector3* pWorldPos, int16_t halfSize, int color, dwRect* pClipRect)
{
    rdVector3 screen;
    this->ProjectPoint(&screen, pWorldPos);
    int16_t x = (int16_t)(int32_t)screen.x; // binary: __ftol (truncate)
    int16_t y = (int16_t)(int32_t)screen.y;

    dwRect rect;
    rect.left = x - halfSize;
    rect.top = y - halfSize;
    rect.right = halfSize + x;
    rect.bottom = halfSize + y;
    if (pClipRect)
        dwRect_Clip(&rect, pClipRect);
    dwImageDraw_FillRect(pDestBits, &rect, color, NULL);
}

// (Ghidra: dwGui3DView_SetLight0)
void dwGui3DView::SetLight0(float x, float y, float z, float intensity, int type)
{
    this->light0Pos.x = x;
    this->light0Pos.y = y;
    this->light0Pos.z = z;
    this->light0.intensity = intensity;
    this->light0.type = type;
    this->RebuildLights(); // vtbl +0x48 (virtual)
}

// (Ghidra: dwGui3DView_SetLight1)
void dwGui3DView::SetLight1(float x, float y, float z, float intensity, int type)
{
    this->light1Pos.x = x;
    this->light1Pos.y = y;
    this->light1Pos.z = z;
    this->light1.intensity = intensity;
    this->light1.type = type;
    this->RebuildLights(); // vtbl +0x48 (virtual)
}
