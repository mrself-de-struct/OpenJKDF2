#ifndef _STD3D_H
#define _STD3D_H

#include "types.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif

#define std3D_Startup_ADDR (0x00429310)
#define std3D_Shutdown_ADDR (0x00429390)
#define std3D_FindClosestDevice_ADDR (0x004293B0)
#define std3D_PurgeTextureCache_ADDR (0x00429750)
#define std3D_GetRenderList_ADDR (0x00429860)
#define std3D_SetRenderList_ADDR (0x00429870)
#define std3D_SetFogColor_ADDR (0x00429880)
#define std3D_SetFogDistances_ADDR (0x004298A0)
#define std3D_GetValidDimensions_ADDR (0x004298C0)
#define std3D_StartScene_ADDR (0x004298F0)
#define std3D_EndScene_ADDR (0x00429900)
#define std3D_ResetRenderList_ADDR (0x00429910)
#define std3D_AddRenderListVertices_ADDR (0x00429970)
#define std3D_RenderListVerticesFinish_ADDR (0x004299D0)
#define std3D_AddRenderListTris_ADDR (0x00429A20)
#define std3D_DrawRenderList_ADDR (0x00429BD0)
#define std3D_AddRenderListTrisAttributes_ADDR (0x00429C80)
#define std3D_SetCurrentPalette_ADDR (0x00429EF0)
#define std3D_GetValidDimension_ADDR (0x00429FA0)
#define std3D_AddToTextureCache_ADDR (0x0042A040)
#define std3D_UnloadAllTextures_ADDR (0x0042A890)
#define std3D_AppendTextureToList_ADDR (0x0042A910)
#define std3D_RemoveTextureFromList_ADDR (0x0042A980)
#define std3D_42AA20_ADDR (0x0042AA20)
#define std3D_UpdateFrameCount_ADDR (0x0042AA90)
#define std3D_ClearZBuffer_ADDR (0x0042AB90)
#define std3D_42AC40_ADDR (0x0042AC40)
#define std3D_FindClosestTextureFormat_ADDR (0x0042ACD0)
#define std3D_DrawOverlay_ADDR (0x0042ADB0)
#define std3D_InitializeViewport_ADDR (0x0042B360)
#define std3D_CreateExecuteBuffer_ADDR (0x0042B450)
#define std3D_CreateViewport_ADDR (0x0042B810)
#define std3D_CreateZBuffer_ADDR (0x0042B920)
#define std3D_EnumerateCallback_ADDR (0x0042BA60)
#define std3D_EnumerateTexturesCallback_ADDR (0x0042BC80)
#define std3D_D3DBitdepthToRdBitdepth_ADDR (0x0042BF50)
#define std3D_RdBitdepthToD3DBitdepth_ADDR (0x0042BF90)
#define std3D_42BFE0_ADDR (0x0042BFE0)
#define std3D_42C030_ADDR (0x0042C030)

extern int std3D_bReinitHudElements;

// Added
int std3D_HasAlpha();
int std3D_HasModulateAlpha();
int std3D_HasAlphaFlatStippled();

#if !defined(SDL2_RENDER) && defined(WIN32)
static int (*std3D_Startup)() = (void*)std3D_Startup_ADDR;
static void (*std3D_Shutdown)() = (void*)std3D_Shutdown_ADDR;
static int (*std3D_StartScene)() = (void*)std3D_StartScene_ADDR;
static int (*std3D_EndScene)() = (void*)std3D_EndScene_ADDR;
static void (*std3D_ResetRenderList)() = (void*)std3D_ResetRenderList_ADDR;
static int (*std3D_RenderListVerticesFinish)() = (void*)std3D_RenderListVerticesFinish_ADDR;
static void (*std3D_DrawRenderList)() = (void*)std3D_DrawRenderList_ADDR;
static int (*std3D_SetCurrentPalette)(rdColor24 *a1, int a2) = (void*)std3D_SetCurrentPalette_ADDR;
static unsigned int* (*std3D_GetValidDimension)(unsigned int a1, unsigned int a2, unsigned int *a3, unsigned int *a4) = (void*)std3D_GetValidDimension_ADDR;;
static int (*std3D_DrawOverlay)() = (void*)std3D_DrawOverlay_ADDR;
static void (*std3D_UnloadAllTextures)() = (void*)std3D_UnloadAllTextures_ADDR;
static void (*std3D_AddRenderListTris)(rdTri *tris, unsigned int num_tris) = (void*)std3D_AddRenderListTris_ADDR;
static int (*std3D_AddRenderListVertices)(D3DVERTEX *vertex_array, int count) = (void*)std3D_AddRenderListVertices_ADDR;
static int (*std3D_ClearZBuffer)() = (void*)std3D_ClearZBuffer_ADDR;
static int (*std3D_AddToTextureCache)(tVBuffer *a1, rdDDrawSurface *tex_2, int is_16bit_maybe, int no_alpha) = (void*)std3D_AddToTextureCache_ADDR;
static void (*std3D_UpdateFrameCount)(rdDDrawSurface *surface) = (void*)std3D_UpdateFrameCount_ADDR;
static void (*std3D_PurgeTextureCache)() = (void*)std3D_PurgeTextureCache_ADDR;
void std3D_InitializeViewport(rdRect *viewRect);
int std3D_GetValidDimensions(int a1, int a2, int a3, int a4);
static int (*std3D_FindClosestDevice)(uint32_t index, int a2) = (void*)std3D_FindClosestDevice_ADDR;
int std3D_SetRenderList(intptr_t a1);
intptr_t std3D_GetRenderList();
static int (*std3D_CreateExecuteBuffer)() = (void*)std3D_CreateExecuteBuffer_ADDR;
#else
int std3D_Startup();
void std3D_Shutdown();
#ifdef TARGET_DREAMCAST
// Diagnostic: paint the TV overscan border to trace where the frame loop is (hangs
// with no console). No-op unless the tracer is enabled (compile -DSTD3D_BORDER_TRACE
// or plug a controller into port 4). See src/Platform/Dreamcast/std3D.c.
void std3D_BorderTrace(uint8_t r, uint8_t g, uint8_t b);
#endif
int std3D_StartScene();
int std3D_EndScene();
void std3D_ResetRenderList();
MATH_FUNC int std3D_RenderListVerticesFinish();
MATH_FUNC void std3D_DrawRenderList();
int std3D_SetCurrentPalette(rdColor24 *a1, int a2);
void std3D_GetValidDimension(unsigned int inW, unsigned int inH, unsigned int *outW, unsigned int *outH);
int std3D_DrawOverlay();
void std3D_UnloadAllTextures();
#ifndef RDCACHE_RENDER_NGONS
MATH_FUNC void std3D_AddRenderListTris(rdTri *tris, unsigned int num_tris);
#else
MATH_FUNC void std3D_AddRenderListNGons(rdNGon *tris, unsigned int num_ngons);
#endif
MATH_FUNC void std3D_AddRenderListLines(rdLine* lines, uint32_t num_lines);
MATH_FUNC int std3D_AddRenderListVertices(D3DVERTEX *vertex_array, int count);
void std3D_UpdateFrameCount(rdDDrawSurface *pTexture);
void std3D_RemoveTextureFromCacheList(rdDDrawSurface *pCacheTexture); // TODO: mark the address for this
void std3D_AddTextureToCacheList(rdDDrawSurface *pTexture); // TODO: mark the address for this
int std3D_PurgeTextureCache(size_t size);
void std3D_PurgeEntireTextureCache();
int std3D_ClearZBuffer();
int std3D_AddToTextureCache(tVBuffer *vbuf, rdDDrawSurface *texture, int is_alpha_tex, int no_alpha);
void std3D_DrawMenu();
void std3D_DrawSceneFbo();
void std3D_FreeResources();
void std3D_InitializeViewport(rdRect *viewRect);
int std3D_GetValidDimensions(int a1, int a2, int a3, int a4);
int std3D_FindClosestDevice(uint32_t index, int a2);
int std3D_SetRenderList(intptr_t a1);
intptr_t std3D_GetRenderList();
int std3D_CreateExecuteBuffer();
#endif

void std3D_PurgeUIEntry(int i, int idx);
void std3D_PurgeTextureEntry(int i);
void std3D_PurgeBitmapRefs(stdBitmap *pBitmap);
void std3D_PurgeSurfaceRefs(rdDDrawSurface *texture);
void std3D_UpdateSettings();
void std3D_Screenshot(const char* pFpath);
#ifdef RDRASTER_SOFTWARE_RENDERER
void std3D_ScreenshotWindow(const char* pFpath);
#endif

void std3D_ResetUIRenderList();
int std3D_AddBitmapToTextureCache(stdBitmap *texture, int mipIdx, int is_alpha_tex, int no_alpha);
void std3D_DrawUIBitmapRGBA(stdBitmap* pBmp, int mipIdx, flex_t dstX, flex_t dstY, rdRect* srcRect, flex_t scaleX, flex_t scaleY, int bAlphaOverwrite, uint8_t color_r, uint8_t color_g, uint8_t color_b, uint8_t color_a);
void std3D_DrawUIBitmap(stdBitmap* pBmp, int mipIdx, flex_t dstX, flex_t dstY, rdRect* srcRect, flex_t scale, int bAlphaOverwrite);
void std3D_DrawUIClearedRect(uint8_t palIdx, rdRect* dstRect);
void std3D_DrawUIClearedRectRGBA(uint8_t color_r, uint8_t color_g, uint8_t color_b, uint8_t color_a, rdRect* dstRect);
int std3D_IsReady();

#ifdef PLATFORM_DROIDWORKS
// Added: DroidWorks workshop readback. Reads the scene FBO region that the DW view rect
// x,y,w,h (640x480 GUI space, mapped by the DW letterbox like the mission view) rendered
// into, box-filtering to one sample per GUI pixel: pOutRgb gets w*h*3 bytes, pOutCover
// gets w*h coverage bytes (0 = nothing was drawn there: depth still at the clear value).
// Returns 0 when there is no scene FBO (headless, backend without one).
int std3D_DwReadbackRect(int x, int y, int w, int h, uint8_t* pOutRgb, uint8_t* pOutCover);
// Added: DroidWorks direct workshop views (the persistent view layer; see std3D.c).
// BeginViewRect: right after the view's std3D_StartScene, seed its rect from the layer.
// CompositeViewRect: after the view's render list flushed, draw the 8-bit GUI backdrop
// (pGuiRect = the canvas pixels under the view, guiStride bytes per row) behind it and
// copy the rect into the layer; the caller then punches the GUI rect to index 0. The
// present blits the layer under the menu quad. ReadbackLayerRect: the layer's pixels
// box-filtered to one RGB sample per GUI pixel (w*h*3 bytes). All return 0 when there
// is no GL scene (headless) or the layer is empty.
int std3D_DwBeginViewRect(int x, int y, int w, int h);
int std3D_DwCompositeViewRect(int x, int y, int w, int h, const uint8_t* pGuiRect, int guiStride);
int std3D_DwReadbackLayerRect(int x, int y, int w, int h, uint8_t* pOutRgb);
void std3D_DwClearViewLayer(void);
#endif

#ifdef __cplusplus
}
#endif


#endif // _STD3D_H
