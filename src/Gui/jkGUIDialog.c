#include "jkGUIDialog.h"

#include "General/Darray.h"
#include "General/stdBitmap.h"
#include "General/stdString.h"
#include "General/stdFont.h"
#include "Engine/rdMaterial.h" // TODO move tVBuffer
#include "stdPlatform.h"
#include "jk.h"
#include "Gui/jkGUIRend.h"
#include "Gui/jkGUI.h"
#include "Main/jkStrings.h"
#include "Win95/stdDisplay.h"
#include "Platform/Common/stdLog.h" // Added
#include "Main/Main.h"

jkGuiElement jkGuiDialog_Ok_buttons[4] = {
    {ELEMENT_TEXT, 0, 2, 0, 3, {0x82, 0x8C, 0x186, 0x28}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_TEXT, 0, 0, 0, 3, {0x82, 0xBE, 0x186, 0x5A}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 1, 2, "GUI_OK", 3, {0x118, 0x122, 0xBE, 0x28}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_END, 0, 0, 0, 0, {0}, 0, 0, 0, 0, 0, 0, {0}, 0}
};

static jkGuiMenu jkGuiDialog_Ok_menu  = {jkGuiDialog_Ok_buttons, -1, 0xE1, 0xFF, 0xF, 0, 0, jkGui_stdBitmaps, jkGui_stdFonts, 0, 0, "thermloop01.wav", "thrmlpu2.wav", 0, 0, 0, 0, 0, 0};

jkGuiElement jkGuiDialog_OkCancel_buttons[5] = {
    {ELEMENT_TEXT, 0, 2, 0, 3, {0x82, 0x8C, 0x186, 0x28}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_TEXT, 0, 0, 0, 3, {0x82, 0xBE, 0x186, 0x5A}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, 1, 2, "GUI_OK", 3, {0x14A, 0x122, 0xBE, 0x28}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, -1, 2, "GUI_CANCEL", 3, {0x82, 0x122, 0xBE, 0x28}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_END, 0, 0, 0, 0, {0}, 0, 0, 0, 0, 0, 0, {0}, 0}
};

jkGuiMenu jkGuiDialog_OkCancel_menu  = {jkGuiDialog_OkCancel_buttons, -1, 0xE1, 0xFF, 0xF, 0, 0, jkGui_stdBitmaps, jkGui_stdFonts, 0, 0, "thermloop01.wav", "thrmlpu2.wav", 0, 0, 0, 0, 0, 0};

tVBuffer *jkGuiDialog_sub_416900();

#ifdef PLATFORM_DPLAY
// Added: a dialog that stays up while something runs (a map or mod download),
// with a status line and Cancel.
static jkGuiElement jkGuiDialog_Progress_buttons[4] = {
    {ELEMENT_TEXT, 0, 2, 0, 3, {0x82, 0x8C, 0x186, 0x28}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_TEXT, 0, 0, 0, 3, {0x82, 0xBE, 0x186, 0x5A}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_TEXTBUTTON, -2, 2, "GUI_CANCEL", 3, {0x118, 0x122, 0xBE, 0x28}, 1, 0, 0, 0, 0, 0, {0}, 0},
    {ELEMENT_END, 0, 0, 0, 0, {0}, 0, 0, 0, 0, 0, 0, {0}, 0}
};

static jkGuiMenu jkGuiDialog_Progress_menu  = {jkGuiDialog_Progress_buttons, -1, 0xE1, 0xFF, 0xF, 0, 0, jkGui_stdBitmaps, jkGui_stdFonts, 0, 0, "thermloop01.wav", "thrmlpu2.wav", 0, 0, 0, 0, 0, 0};

static int (*jkGuiDialog_progressTick)(void* ctx, char16_t* text, int maxChars);
static void* jkGuiDialog_progressCtx;
static char16_t jkGuiDialog_progressText[2][160];
static int jkGuiDialog_progressShown;
static int jkGuiDialog_progressResult;

static void jkGuiDialog_ProgressIdle(jkGuiMenu* pMenu)
{
    char16_t* next = jkGuiDialog_progressText[!jkGuiDialog_progressShown];
    int r;
    next[0] = 0;
    r = jkGuiDialog_progressTick(jkGuiDialog_progressCtx, next, 160);
    if (r)
    {
        jkGuiDialog_progressResult = r;
        pMenu->lastClicked = 1;
        return;
    }
    if (next[0] && __wcscmp(next, jkGuiDialog_progressText[jkGuiDialog_progressShown]))
    {
        jkGuiDialog_progressShown = !jkGuiDialog_progressShown;
        jkGuiDialog_Progress_buttons[1].wstr = next;
        jkGuiRend_UpdateAndDrawClickable(&jkGuiDialog_Progress_buttons[1], pMenu, 1);
    }
}

// Keeps the dialog up while tick() returns 0; tick() writes the status line
// and returns 1 when done or -1 on failure. Returns that, or -2 if the player
// cancelled.
int jkGuiDialog_ProgressDialog(char16_t* title, int (*tick)(void* ctx, char16_t* text, int maxChars), void* ctx)
{
    int v2 = 0, r;

    jkGuiDialog_progressTick = tick;
    jkGuiDialog_progressCtx = ctx;
    jkGuiDialog_progressShown = 0;
    jkGuiDialog_progressResult = 0;
    jkGuiDialog_progressText[0][0] = 0;

    // Done before anything is drawn (e.g. nothing to download): no dialog.
    r = tick(ctx, jkGuiDialog_progressText[0], 160);
    if (r) return r;

    stdBitmap_EnsureData(jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]);
    if ( !jkGui_GdiMode )
    {
        jkGui_SetModeMenu(jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]->palette);
        v2 = 1;
    }
    jkGuiDialog_Progress_buttons[0].wstr = title;
    jkGuiDialog_Progress_buttons[1].wstr = jkGuiDialog_progressText[0];
#ifdef STDBITMAP_PARTIAL_LOAD
    jkGuiDialog_Progress_menu.pTextureOverride = jkGuiDialog_sub_416900();
    jkGuiDialog_Progress_menu.pBgBitmap = NULL;
#else
    jkGuiDialog_Progress_menu.texture = jkGuiDialog_sub_416900();
    jkGuiDialog_Progress_menu.palette = NULL;
#endif
    jkGuiDialog_Progress_buttons[2].wstr = jkStrings_GetUniStringWithFallback("GUI_CANCEL");
    jkGuiDialog_Progress_menu.idkFunc = jkGuiDialog_ProgressIdle;
    jkGuiRend_MenuSetEscapeKeyShortcutElement(&jkGuiDialog_Progress_menu, &jkGuiDialog_Progress_buttons[2]);
    r = jkGuiRend_DisplayAndReturnClicked(&jkGuiDialog_Progress_menu);

#ifdef STDBITMAP_PARTIAL_LOAD
    stdDisplay_VBufferFree(jkGuiDialog_Progress_menu.pTextureOverride);
    jkGuiDialog_Progress_menu.pTextureOverride = NULL;
#else
    stdDisplay_VBufferFree(jkGuiDialog_Progress_menu.texture);
    jkGuiDialog_Progress_menu.texture = NULL;
#endif
    if ( v2 )
        jkGui_SetModeGame();
    stdBitmap_UnloadData(jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]);

    return r == 1 ? jkGuiDialog_progressResult : -2;
}
#endif

void jkGuiDialog_Startup()
{
    jkGui_InitMenu(&jkGuiDialog_OkCancel_menu, 0);
    jkGui_InitMenu(&jkGuiDialog_Ok_menu, 0);
#ifdef PLATFORM_DPLAY
    jkGui_InitMenu(&jkGuiDialog_Progress_menu, 0); // Added
#endif
}

void jkGuiDialog_Shutdown()
{
    stdPlatform_Printf("OpenJKDF2: %s\n", __func__); // Added
}

tVBuffer *jkGuiDialog_sub_416900()
{
    tVBuffer *v0; // eax
    tVBuffer *v1; // esi

    // Added
    stdBitmap_EnsureData(jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]);
    stdBitmap_EnsureData(jkGui_stdBitmaps[JKGUI_BM_BK_DIALOG]);

    v0 = stdDisplay_VBufferNew(&(*jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]->mipSurfaces)->format, 0, 0, Video_menuBuffer.palette);
    v1 = v0;
    if ( v0 )
    {
        int copyX = jkGui_stdBitmaps[JKGUI_BM_BK_DIALOG]->xPos;
        int copyY = jkGui_stdBitmaps[JKGUI_BM_BK_DIALOG]->yPos;
#ifdef JKGUI_SMOL_SCREEN
        copyX = (int)((flex_t)copyX * 0.4);
        copyY = (int)((flex_t)copyY * 0.4);
#endif
        stdDisplay_VBufferCopy(v0, &Video_menuBuffer, 0, 0, 0, 0);
        stdDisplay_VBufferCopy(v1, *jkGui_stdBitmaps[JKGUI_BM_BK_DIALOG]->mipSurfaces, copyX, copyY, 0, 0);
    }

    stdBitmap_UnloadData(jkGui_stdBitmaps[JKGUI_BM_BK_DIALOG]);

    return v1;
}

// Added: dialogs go into the log too (an unattended server can't see them).
static void jkGuiDialog_Log(const char* pKind, const char16_t* pTitle, const char16_t* pText)
{
    uint16_t aLine[512];
    size_t at = 0;
    const char* pSep = ": ";
    for (size_t i = 0; pKind[i] && at < 500; i++) aLine[at++] = (uint8_t)pKind[i];
    aLine[at++] = ' ';
    for (size_t i = 0; pTitle && pTitle[i] && at < 500; i++) aLine[at++] = pTitle[i];
    for (size_t i = 0; pSep[i] && at < 500; i++) aLine[at++] = (uint8_t)pSep[i];
    for (size_t i = 0; pText && pText[i] && at < 510; i++) aLine[at++] = (pText[i] == '\n') ? ' ' : pText[i];
    aLine[at] = 0;
    stdLog_PrintW("dialog", aLine);
}

int jkGuiDialog_OkCancelDialog(char16_t *stringA, char16_t *stringB)
{
    int v2; // edi
    int v5; // esi

    jkGuiDialog_Log("OK/Cancel", stringA, stringB); // Added
#ifdef QOL_IMPROVEMENTS
    // Added: a dedicated server has nobody to click it
    if (Main_bDedicatedServer) {
        stdLog_Printf("dialog", "answered Cancel (dedicated server)");
        return 0;
    }
#endif

    // Added
    stdBitmap_EnsureData(jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]);

    v2 = 0;
    if ( !jkGui_GdiMode )
    {
        jkGui_SetModeMenu(jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]->palette);
        v2 = 1;
    }
    jkGuiDialog_OkCancel_buttons[0].wstr = stringA;
    jkGuiDialog_OkCancel_buttons[1].wstr = stringB;
#ifdef STDBITMAP_PARTIAL_LOAD
    jkGuiDialog_OkCancel_menu.pTextureOverride = jkGuiDialog_sub_416900();
    jkGuiDialog_OkCancel_menu.pBgBitmap = NULL;
#else
    jkGuiDialog_OkCancel_menu.texture = jkGuiDialog_sub_416900();
    jkGuiDialog_OkCancel_menu.palette = NULL;
#endif
    jkGuiDialog_OkCancel_buttons[2].wstr = jkStrings_GetUniStringWithFallback("GUI_OK");
    jkGuiDialog_OkCancel_buttons[3].bIsVisible = 1;
    jkGuiRend_MenuSetReturnKeyShortcutElement(&jkGuiDialog_OkCancel_menu, &jkGuiDialog_OkCancel_buttons[2]);
    jkGuiRend_MenuSetEscapeKeyShortcutElement(&jkGuiDialog_OkCancel_menu, &jkGuiDialog_OkCancel_buttons[3]);
    v5 = jkGuiRend_DisplayAndReturnClicked(&jkGuiDialog_OkCancel_menu);
    
#ifdef STDBITMAP_PARTIAL_LOAD
    stdDisplay_VBufferFree(jkGuiDialog_OkCancel_menu.pTextureOverride);
    jkGuiDialog_OkCancel_menu.pTextureOverride = NULL;
#else
    stdDisplay_VBufferFree(jkGuiDialog_OkCancel_menu.texture);
    jkGuiDialog_OkCancel_menu.texture = NULL;
#endif
    if ( v2 )
        jkGui_SetModeGame();

    // Added
    stdBitmap_UnloadData(jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]);

    return v5 == 1;
}

void jkGuiDialog_ErrorDialog(char16_t *stringA, char16_t *stringB)
{
    int v2; // edi

    jkGuiDialog_Log("error", stringA, stringB); // Added
#ifdef QOL_IMPROVEMENTS
    // Added: a dedicated server has nobody to click it
    if (Main_bDedicatedServer) {
        stdLog_Printf("dialog", "answered OK (dedicated server)");
        return;
    }
#endif

    // Added
    stdBitmap_EnsureData(jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]);

    v2 = 0;
    if ( !jkGui_GdiMode )
    {
        jkGui_SetModeMenu(jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]->palette);
        v2 = 1;
    }
    jkGuiDialog_Ok_buttons[0].wstr = stringA;
    jkGuiDialog_Ok_buttons[1].wstr = stringB;
#ifdef STDBITMAP_PARTIAL_LOAD
    jkGuiDialog_Ok_menu.pTextureOverride = jkGuiDialog_sub_416900();
    jkGuiDialog_Ok_menu.pBgBitmap = NULL;
#else
    jkGuiDialog_Ok_menu.texture = jkGuiDialog_sub_416900();
    jkGuiDialog_Ok_menu.palette = NULL;
#endif
    jkGuiDialog_Ok_buttons[2].wstr = jkStrings_GetUniStringWithFallback("GUI_OK");
    jkGuiRend_MenuSetReturnKeyShortcutElement(&jkGuiDialog_Ok_menu, &jkGuiDialog_Ok_buttons[2]);
    jkGuiRend_MenuSetEscapeKeyShortcutElement(&jkGuiDialog_Ok_menu, &jkGuiDialog_Ok_buttons[2]);
    jkGuiRend_DisplayAndReturnClicked(&jkGuiDialog_Ok_menu);
    
#ifdef STDBITMAP_PARTIAL_LOAD
    stdDisplay_VBufferFree(jkGuiDialog_Ok_menu.pTextureOverride);
    jkGuiDialog_Ok_menu.pTextureOverride = NULL;
#else
    stdDisplay_VBufferFree(jkGuiDialog_Ok_menu.texture);
    jkGuiDialog_Ok_menu.texture = NULL;
#endif
    if ( v2 )
        jkGui_SetModeGame();

    // Added
    stdBitmap_UnloadData(jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]);
}

int jkGuiDialog_YesNoDialog(char16_t *stringA, char16_t *stringB)
{
    int v2; // edi
    int v5; // esi

    jkGuiDialog_Log("yes/no", stringA, stringB); // Added
#ifdef QOL_IMPROVEMENTS
    // Added: a dedicated server has nobody to click it
    if (Main_bDedicatedServer) {
        stdLog_Printf("dialog", "answered No (dedicated server)");
        return 0;
    }
#endif

    // Added
    stdBitmap_EnsureData(jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]);

    v2 = 0;
    if ( !jkGui_GdiMode )
    {
        jkGui_SetModeMenu(jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]->palette);
        v2 = 1;
    }
    jkGuiDialog_OkCancel_buttons[0].wstr = stringA;
    jkGuiDialog_OkCancel_buttons[1].wstr = stringB;
#ifdef STDBITMAP_PARTIAL_LOAD
    jkGuiDialog_OkCancel_menu.pTextureOverride = jkGuiDialog_sub_416900();
    jkGuiDialog_OkCancel_menu.pBgBitmap = NULL;
#else
    jkGuiDialog_OkCancel_menu.texture = jkGuiDialog_sub_416900();
    jkGuiDialog_OkCancel_menu.palette = NULL;
#endif
    jkGuiDialog_OkCancel_buttons[2].wstr = jkStrings_GetUniStringWithFallback("GUI_YES");
    jkGuiDialog_OkCancel_buttons[3].wstr = jkStrings_GetUniStringWithFallback("GUI_NO");
    jkGuiDialog_OkCancel_buttons[3].bIsVisible = 1;
    jkGuiRend_MenuSetReturnKeyShortcutElement(&jkGuiDialog_OkCancel_menu, &jkGuiDialog_OkCancel_buttons[2]);
    jkGuiRend_MenuSetEscapeKeyShortcutElement(&jkGuiDialog_OkCancel_menu, &jkGuiDialog_OkCancel_buttons[3]);
    v5 = jkGuiRend_DisplayAndReturnClicked(&jkGuiDialog_OkCancel_menu);
    
#ifdef STDBITMAP_PARTIAL_LOAD
    stdDisplay_VBufferFree(jkGuiDialog_OkCancel_menu.pTextureOverride);
    jkGuiDialog_OkCancel_menu.pTextureOverride = NULL;
#else
    stdDisplay_VBufferFree(jkGuiDialog_OkCancel_menu.texture);
    jkGuiDialog_OkCancel_menu.texture = NULL;
#endif
    if ( v2 )
        jkGui_SetModeGame();

    // Added
    stdBitmap_UnloadData(jkGui_stdBitmaps[JKGUI_BM_BK_MAIN]);

    return v5 == 1;
}
