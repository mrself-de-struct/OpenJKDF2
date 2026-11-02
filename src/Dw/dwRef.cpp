// dwRef — reference-room content controls (dwRefGraph / dwRefPulldown /
// dwRefMenuItem). See Dw/dwRef.h for the class roster and design notes.
//
// Decompiled from DroidWorks.exe. Built by
// dwGuiReference_CreateControl (keywords GRAPH / PULLDOWN_MENU). Everything
// verifiably C++ (vtables, ctor/dtor pairs, MSVC EH frames).

#include "Dw/dwRef.h"

#include "Dw/dwGuiHypText.h" // dwRefGraph_LayoutLabels builds Arial10 labels
#include "Dw/dwString.h"
#include "Dw/dwList.h"
#include "Dw/dwFont.h"
#include "Dw/dwImage.h"      // dwImage_LoadFile (dwRefPulldown images)
#include "Dw/dwImageDraw.h"  // BlendRect / FrameRect (dwRefPulldown_Draw)
#include "Dw/dwColormap.h"   // dwColormap_transparentIdx
#include "Dw/dwConfFile.h"
#include "Dw/dwInits.h"      // inits_EnumFilesByExt
#include "Dw/dwSound.h"
#include "Dw/dwWidget.h"     // dwWidget_pMouseTarget, DrawChild
#include "Dw/dwRect.h"

#include "jk.h"

extern "C" {
#include "Win95/stdDisplay.h" // stdDisplay_pCurVideoMode: mode dims
}

#include <stdlib.h> // free

// ---- unresolved cross-unit externs ---------------------------------------------

// dwGuiPicture: the shared id-keyed image-swap picture box (a dwWidgetGroup
// subclass). Used by dwRefGraph_LayoutMarkers for the ROrange/RGreen/RBlue
// arrow markers. Not yet landed.
// TODO(dw-decomp): provided by dwGuiStatus (dwGuiPicture COMDAT). Binary:
// new(0x2c) dwGuiPicture_Ctor(&rect, pImgName, id).
extern dwWidget* dwGuiPicture_New(dwRect* pRect, const char* pImgName, int id);

// ---- module init ----------------------------------------------------------------

extern "C" void dwRef_Startup(void)
{
    // No module statics.
}

// ---- shared list helpers -------------------------------------------------------

// Append pData at the tail of pList (mirrors dwRefGraph_ListAppend and
// the many inlined tail-inserts across the unit).
static void dwRef_Append(dwList* pList, void* pData)
{
    pList->InsertAfter(pList->pSentinel->pPrev, pData);
}

// Free a list of dwString* payloads (topic-file records from
// inits_EnumFilesByExt): unlink each node, delete its dwString, then free the
// sentinel. Mirrors dwRefGraph_FreeTopicList (payload = the filename
// dwString the enumerator allocated).
static void dwRef_FreeStringList(dwList* pList)
{
    dwListNode* pSent = pList->pSentinel;
    dwListNode* pNode = pSent->pNext;
    while (pNode != pSent)
    {
        dwString* pStr = (dwString*)pNode->pData;
        dwListNode* pNext = pNode->pNext;
        pList->UnlinkFreeNode(pNode);
        if (pStr != NULL)
            delete pStr; // dwString dtor = Free(); matches dwString_Free + FreeHandle
        pNode = pNext;
    }
    pList->Free();
}

// =================================================================================
//  dwRefGraph
// =================================================================================

// dwRefGraph_Ctor — the three data lists are constructed by their
// member initializers (each allocs its sentinel, matching the binary's manual
// sentinel allocs). LoadTopics/LayoutLabels/LayoutMarkers run at construction.
dwRefGraph::dwRefGraph(dwRect* pRect, void* pScreen, float config1, float config2,
                       char* pSoundName)
    : dwWidgetGroup(pRect)
    , dataPointCount(0)
    , pScreen(pScreen)
    , viewMode(0)
    , config1(config1)
    , config2(config2)
    , animTime(0.0f)
    , soundName(pSoundName, 0)
    , bDirtyX(0)
    , bDirtyY(0)
{
    // The reveal rect starts empty at the bottom-left corner (binary: {left, bottom, left, bottom}).
    this->animOriginX = this->left;
    this->curY = this->bottom;
    this->curX = this->left;
    this->targetY = this->bottom;

    this->LoadTopics();
    this->LayoutLabels();
    this->LayoutMarkers();
}

// dwRefGraph_Dtor — free the data + topic lists (the base
// dwWidgetGroup dtor deletes the label/marker children and frees the child
// list). soundName is freed by its member dtor.
dwRefGraph::~dwRefGraph()
{
    // pDataPoints payloads are dwRefGraphPoint* (delete = dwString name dtor +
    // free), matching dwRefGraph_FreeStringNode.
    dwListNode* pSent = this->pDataPoints.pSentinel;
    dwListNode* pNode = pSent->pNext;
    while (pNode != pSent)
    {
        dwRefGraphPoint* pPt = (dwRefGraphPoint*)pNode->pData;
        dwListNode* pNext = pNode->pNext;
        this->pDataPoints.UnlinkFreeNode(pNode);
        if (pPt != NULL)
            delete pPt;
        pNode = pNext;
    }
    this->pDataPoints.Free();

    dwRef_FreeStringList(&this->pTopicsSUB);
    dwRef_FreeStringList(&this->pTopicsTPC);
}

// dwRefGraph_AddDataPoint — append a { name, density, strength }
// record at the tail of pDataPoints.
void dwRefGraph::AddDataPoint(char* pName, float density, float strength)
{
    dwRefGraphPoint* pPt = new dwRefGraphPoint(pName, density, strength);
    dwRef_Append(&this->pDataPoints, pPt);
    this->dataPointCount++;
}

// handled inline in the header (SetMode0/SetMode1).

// dwRefGraph_Update — while enabled, grow the reveal rect toward the widget rect over one
// second (the x87 math: delta = __ftol(span * animTime), clamped to the span), toggling the
// tick sound each frame while it moves and stopping it once it arrives; then the group tick.
void dwRefGraph::Update(float dt)
{
    if (this->bEnabled != 0)
    {
        this->animTime += dt;
        if (this->curX != this->right || this->curY != this->top)
        {
            int spanX = (int16_t)(this->right - this->left);
            int spanY = (int16_t)(this->bottom - this->top);
            int dx = (int)((float)spanX * this->animTime);
            int dy = (int)((float)spanY * this->animTime);
            if ((uint16_t)dx > (uint16_t)spanX) dx = spanX;
            this->bDirtyX = 1;
            if ((uint16_t)dy > (uint16_t)spanY) dy = spanY;
            this->bDirtyY = 1;

            if (dwSound_IsPlaying(this->soundName.pBuffer) != 0)
                dwSound_Stop(this->soundName.pBuffer);
            else
                dwSound_PlayLooping(this->soundName.pBuffer);

            this->curX = (int16_t)(this->animOriginX + dx);
            this->curY = (int16_t)(this->targetY - dy);
            if (this->curX == this->right && this->curY == this->top &&
                dwSound_IsPlaying(this->soundName.pBuffer) != 0)
            {
                dwSound_Stop(this->soundName.pBuffer);
            }
            this->Invalidate();
        }
    }
    dwWidgetGroup::Update(dt);
}

// Bar height rule shared by the current-topic bar and the data-point bars (binary
// 0x416356 / 0x4164b0): the bar's top moves DOWN from the chart top by H / value, so a
// bigger value makes a taller bar. Density: 0 -> flat, 1.0 -> 10, else H / (int)v.
// Strength: 0 -> flat, 1.0 -> 10, v <= 200 -> H / (int)v + 100, else H / ((int)v / 100)
// (10 when that rounds to nothing). (int)v == 0 would divide by zero in the original;
// treated as 1 here.
static int dwRefGraph_BarDrop(int mode, float v, int H)
{
    if (v == 0.0f) return 0;
    if (v == 1.0f) return 10;
    int iv = (int)v;
    if (mode == 0)
        return H / (iv ? iv : 1);
    if (v <= 200.0f)
        return H / (iv ? iv : 1) + 100;
    int d = iv / 100;
    int r = H / (d ? d : 1);
    return (int16_t)r != 0 ? r : 10;
}

void dwRefGraph::Draw(dwImageBits* pDestBits, dwRect* pClipRect)
{
    // Everything is clipped to the reveal rect (0x34..0x3a) so the chart wipes in.
    dwRect reveal;
    reveal.left = this->animOriginX; reveal.top = this->curY;
    reveal.right = this->curX; reveal.bottom = this->targetY;
    dwRect clip = *pClipRect;
    dwRect_Clip(&clip, &reveal);

    // The current topic's own bar: x = left+10..left+30, color 2.
    dwRect bar;
    bar.left = (int16_t)(this->left + 10);
    bar.top = this->top;
    bar.right = (int16_t)(this->left + 30);
    bar.bottom = (int16_t)(this->bottom - 120);
    int H = (int16_t)(bar.bottom - bar.top);
    bar.top = (int16_t)(bar.top + dwRefGraph_BarDrop(this->viewMode, this->viewMode == 0 ? this->config1 : this->config2, H));
    dwImageDraw_FillRect(pDestBits, &bar, 2, &clip);

    // One 15 px bar per data point, every 21 px from left+6, colors 0xe5 / 0xc1 / 0x20.
    static const int aColor[3] = { 0xe5, 0xc1, 0x20 };
    char state = 0;
    bar.left = this->left;
    bar.right = (int16_t)(this->left + 30);
    dwListNode* pSent = this->pDataPoints.pSentinel;
    for (dwListNode* pNode = pSent->pNext; pNode != pSent; pNode = pNode->pNext)
    {
        dwRefGraphPoint* pPt = (dwRefGraphPoint*)pNode->pData;
        bar.left = (int16_t)(bar.left + 6);
        bar.right = (int16_t)(bar.left + 15);
        bar.top = this->top;
        bar.bottom = (int16_t)(this->bottom - 120);
        H = (int16_t)(bar.bottom - bar.top);
        bar.top = (int16_t)(bar.top + dwRefGraph_BarDrop(this->viewMode, this->viewMode == 0 ? pPt->density : pPt->strength, H));
        dwImageDraw_FillRect(pDestBits, &bar, aColor[(int)state], &clip);
        state = (char)((state + 1) % 3);
        bar.left = bar.right;
    }

    dwWidgetGroup::Draw(pDestBits, &clip);
}

// dwRefGraph_OnMessage — 0x1b62 SetMode0 / 0x1b63 SetMode1: re-seed
// the eased rect from the widget rect, re-tick and invalidate.
int dwRefGraph::OnMessage(dwWidgetMsg* pMsg)
{
    if (pMsg->code == 0x1b62)
        this->SetMode0();
    else if (pMsg->code == 0x1b63)
        this->SetMode1();
    else
        return 0;

    // Restart the reveal: right back to left, top back to bottom.
    this->curX = this->left;
    this->curY = this->bottom;
    this->Update(0.0f);
    this->Invalidate();
    return 1;
}

// dwRefGraph_LoadTopics — enumerate TPC + SUB topic files and parse
// each whose FREQUENCY line is non-zero.
void dwRefGraph::LoadTopics()
{
    inits_EnumFilesByExt("TPC", &this->pTopicsTPC);
    inits_EnumFilesByExt("SUB", &this->pTopicsSUB);

    // For every enumerated topic file, scan for its FREQUENCY line; when it is
    // non-zero, ParseTopicFile reopens the file and reads TOPIC_NAME/DENSITY/
    // STRENGTH (the binary opens the file to probe FREQUENCY, then ParseTopicFile
    // opens it again — reproduced faithfully here since ParseTopicFile owns its
    // open/close).
    dwList* aLists[2] = { &this->pTopicsTPC, &this->pTopicsSUB };
    for (int li = 0; li < 2; li++)
    {
        dwListNode* pSent = aLists[li]->pSentinel;
        for (dwListNode* pNode = pSent->pNext; pNode != pSent; pNode = pNode->pNext)
        {
            dwConfFile probe;
            const char* pPath = ((dwString*)pNode->pData)->pBuffer;
            int bFreq = 0;
            dwConfFile_Open(&probe, pPath);
            while (probe.bEof == 0)
            {
                dwConfFile_ReadLine(&probe);
                char* pTok = dwConfFile_NextToken(&probe);
                if (dwString_Equals(pTok, "FREQUENCY"))
                {
                    uint32_t freq = 0;
                    dwConfFile_ParseULong(&probe, &freq);
                    bFreq = (freq != 0);
                    break;
                }
            }
            dwConfFile_Close(&probe);
            if (bFreq)
            {
                dwConfFile conf;
                dwConfFile_Open(&conf, pPath);
                this->ParseTopicFile(&conf);
            }
        }
    }
}

// dwRefGraph_ParseTopicFile — read TOPIC_NAME/DENSITY/STRENGTH from an
// OPEN conf, then AddDataPoint and close. (See LoadTopics for the open flow.)
void dwRefGraph::ParseTopicFile(dwConfFile* pConf)
{
    if (pConf == NULL)
        return; // guard for the first-pass placeholder call in LoadTopics

    dwString name;
    float density = 0.0f;
    float strength = 0.0f;

    while (pConf->bEof == 0)
    {
        dwConfFile_ReadLine(pConf);
        char* pTok = dwConfFile_NextToken(pConf);
        if (dwString_Equals(pTok, "TOPIC_NAME"))
            name.Assign(pConf->pCursor, 0);
        else if (dwString_Equals(pTok, "DENSITY"))
            dwConfFile_ParseFloat(pConf, &density);
        else if (dwString_Equals(pTok, "STRENGTH"))
            dwConfFile_ParseFloat(pConf, &strength);
    }
    this->AddDataPoint(name.pBuffer, density, strength);
    dwConfFile_Close(pConf);
}

// dwRefGraph_LayoutLabels — the material-name labels (Arial10 dwGuiHypText children).
// Re-read from the binary: the labels run down a diagonal — each one 21 px right and 20 px
// down from the last, every third one back up at y=340 — in three cycling colors, with a
// few hard-coded x positions nudged to line up with the chart art. The rect passed to the
// hyptext is {x, y, x + len + w, 400}, x starting at rect.left + 36 (verbatim quirks:
// the name LENGTH is added to the x advance alongside half the pixel width).
void dwRefGraph::LayoutLabels()
{
    dwFont* pFont = dwFont_Load((dwFont*)operator new(sizeof(dwFont)), "Arial10");

    int16_t x = (int16_t)(this->left + 0x24);   // [esp+0x28]
    int16_t y = 0x154;                          // [esp+0x2a]
    int16_t nextX = x, nextY = y;               // [esp+0x20]/[esp+0x22]
    char state = 0;

    dwListNode* pSent = this->pDataPoints.pSentinel;
    for (dwListNode* pNode = pSent->pNext; pNode != pSent; pNode = pNode->pNext)
    {
        dwRefGraphPoint* pPt = (dwRefGraphPoint*)pNode->pData;
        int len = (int)pPt->name.length;
        int16_t w = (int16_t)dwFont_MeasureString(pFont, pPt->name.pBuffer, len);
        uint8_t color;

        if (state == 0)
        {
            if (x == 0xf3)       x = (int16_t)(0xf3 - w / 2 - len);
            else if (x == 0x132) x = (int16_t)(0x134 - w / 2);
            else if (x == 0x171) x = (int16_t)(0x176 - w / 2);
            else                 x = (int16_t)(x + (len - w / 2));
            color = 0xe5;
        }
        else if (state == 1)
        {
            if (x == 0x147)      x = (int16_t)(len - w / 2 + 0x149);
            else if (x == 0x108) x = (int16_t)(0x116 - w / 2);
            else                 x = (int16_t)(x + (len - w / 2) - 0xe);
            color = 0xc1;
        }
        else
        {
            x = (int16_t)(x + (len - w / 2));
            color = 0x20;
        }

        dwRect r;
        r.left = x; r.top = y; r.right = (int16_t)(len + w + x); r.bottom = 0x190;
        dwGuiHypText* pLabel = new dwGuiHypText(&r, NULL, (char*)"Arial10", color, (char*)"DLO");
        pLabel->text.Free();
        pLabel->SetText(pPt->name.pBuffer);
        dwRef_Append(&this->children, pLabel);

        if (state == 2)
        {
            nextX = (int16_t)(nextX + 0x15);
            nextY = 0x154;
            state = 0;
        }
        else
        {
            nextX = (int16_t)(nextX + 0x15);
            nextY = (int16_t)(nextY + 0x14);
            state++;
        }
        // The binary carries the x/y deltas into the working rect: x/y take the next slot.
        x = nextX;
        y = nextY;
    }
    operator delete(pFont); // the handle only; the glyph block belongs to dwFont's cache
}

// dwRefGraph_LayoutMarkers — the colored arrow markers (ROrange/RGreen/RBlueArrow.RLE
// dwGuiPicture children), one per data point, stepping 21 px right along the chart's
// baseline (rect.bottom - 135), the green ones 4 px and the blue ones 14 px higher.
void dwRefGraph::LayoutMarkers()
{
    int16_t baseTop = (int16_t)(this->bottom - 0x87);
    dwRect r;
    r.left = 0xb2; r.top = baseTop; r.right = 0xd0; r.bottom = (int16_t)(baseTop + 0x64);
    char state = 0;

    static const char* const aArrow[3] = { "ROrangeArrow.RLE", "RGreenArrow.RLE", "RBlueArrow.RLE" };
    static const int16_t aLift[3] = { 0, 4, 0xe };

    dwListNode* pSent = this->pDataPoints.pSentinel;
    for (dwListNode* pNode = pSent->pNext; pNode != pSent; pNode = pNode->pNext)
    {
        dwRect m = r;
        m.top = (int16_t)(r.top - aLift[(int)state]);
        m.bottom = (int16_t)(r.bottom - aLift[(int)state]);
        dwWidget* pPic = dwGuiPicture_New(&m, aArrow[(int)state], 0);
        if (pPic != NULL)
            dwRef_Append(&this->children, pPic);

        r.left = (int16_t)(r.left + 0x15);
        r.right = (int16_t)(r.left + 0x23);
        state = (char)((state + 1) % 3);
    }
}

// =================================================================================
//  dwRefMenuItem
// =================================================================================

dwRefMenuItem::dwRefMenuItem(const char* pTopicFile, const char* pName, const char* pCategory,
                             int value, uint8_t flag, dwRefPulldown* pSubmenu)
    : topicFile(pTopicFile, 0)
    , name(pName, 0)
    , category(pCategory, 0)
    , value(value)
    , flag(flag)
    , pSubmenu(pSubmenu)
{
}

// dwRefMenuItem_Dtor — delete any submenu, then the strings (member
// dtors here).
dwRefMenuItem::~dwRefMenuItem()
{
    if (this->pSubmenu != NULL)
        delete this->pSubmenu;
}

// =================================================================================
//  dwRefPulldown
// =================================================================================

// dwRefPulldown_Ctor — the three lists are member-constructed (each
// with its sentinel). A root menu precaches its images + snapshots its
// collapsed rect; a submenu starts already expanded/open.
dwRefPulldown::dwRefPulldown(dwRect* pRect, char* pFontName, uint8_t colorParam,
                             uint8_t hotColorParam, uint32_t labelColor, char* pImgNormal,
                             char* pImgHot, char* pOpenWav, char* pSelectWav, char* pCloseWav,
                             char* pHoverWav, char* pLabel, uint8_t textColor, char* pFontHot,
                             uint8_t bIsSubmenu)
    : dwWidget(pRect)
    , bLayingOut(1)
    , pRootMenu(NULL)
    , field_0x18(0)
    , field_0x1c(colorParam)
    , field_0x1d(0)
    , field_0x1e(hotColorParam)
    , pImageNormal(NULL)
    , imageNameNormal()
    , pImagePressed(NULL)
    , imageNameHot()
    , pSelectedItem(NULL)
    , field_0x44(0)
    , pFontNormal(NULL)
    , field_0x4c(labelColor)
    , bExpanded(0)
    , bOpen(0)
    , label(pLabel, 0)
    , pVisibleHead(NULL)
    , width(0)
    , topicScratch()
    , field_0x80(0)
    , expandHeight(0)
    , rectExpandLeft(0), rectExpandTop(0), rectExpandRight(0), rectExpandBottom(0)
    , savedLeft(0), savedTop(0), savedRight(0), savedBottom(0)
    , field_0x94(0), field_0x96(0), field_0x98(0), field_0x9a(0)
    , field_0x9c(0), field_0x9e(0), field_0xa0(0), field_0xa2(0)
    , field_0xa4(pFontName, 0)
    , field_0xb0(pImgNormal, 0)
    , field_0xbc(pImgHot, 0)
    , bIsSubmenu(bIsSubmenu)
    , field_0xca(0)
    , field_0xcc(0)
    , field_0xcd(textColor)
    , pFontHot(NULL)
    , field_0xd4(pHoverWav, 0)   // param_11 -> field_0xd4 (hover-enter wav)
    , field_0xe0(pCloseWav, 0)   // param_10 -> field_0xe0 (open/close wav)
    , field_0xec(pOpenWav, 0)    // param_8  -> field_0xec (select wav)
    , field_0xf8(pSelectWav, 0)  // param_9  -> field_0xf8 (close wav)
{
    if (pFontName != NULL)
        this->pFontNormal = dwFont_Load((dwFont*)operator new(sizeof(dwFont)), pFontName);
    if (pFontHot != NULL)
        this->pFontHot = dwFont_Load((dwFont*)operator new(sizeof(dwFont)), pFontHot);

    if (this->bIsSubmenu == 0)
    {
        this->pRootMenu = (void*)this;
        this->imageNameNormal.AssignCStr(pImgNormal);
        this->imageNameHot.AssignCStr(pImgHot);
        this->EnsureImages();
        this->BuildItems();
        this->savedLeft = this->left;
        this->savedTop = this->top;
        this->savedRight = this->right;
        this->savedBottom = this->bottom;
    }
    else
    {
        this->BuildItems();
        this->bOpen = 1;
        this->bExpanded = 1;
        this->Invalidate();
    }
}

// dwRefPulldown_Dtor — free images, fonts, the three lists (item list
// deletes dwRefMenuItem payloads; the TPC/SUB lists delete their dwString
// filename payloads), then the strings (member dtors).
dwRefPulldown::~dwRefPulldown()
{
    this->FreeImages();

    if (this->bIsSubmenu == 0)
    {
        if (this->pFontNormal != NULL) { operator delete(this->pFontNormal); } // allocated with operator new
        if (this->pFontHot != NULL)    { operator delete(this->pFontHot); }
    }

    // pItems payloads are dwRefMenuItem* (delete = full item dtor).
    dwListNode* pSent = this->pItems.pSentinel;
    dwListNode* pNode = pSent->pNext;
    while (pNode != pSent)
    {
        dwRefMenuItem* pItem = (dwRefMenuItem*)pNode->pData;
        dwListNode* pNext = pNode->pNext;
        this->pItems.UnlinkFreeNode(pNode);
        if (pItem != NULL)
            delete pItem;
        pNode = pNext;
    }
    this->pItems.Free();

    dwRef_FreeStringList(&this->pListB);
    dwRef_FreeStringList(&this->pListC);
}

// dwRefPulldown_EnsureImages
void dwRefPulldown::EnsureImages()
{
    if (this->pImageNormal == NULL && this->imageNameNormal.length != 0)
        this->pImageNormal = dwImage_LoadFile(this->imageNameNormal.pBuffer);
    if (this->pImagePressed == NULL && this->imageNameHot.length != 0)
        this->pImagePressed = dwImage_LoadFile(this->imageNameHot.pBuffer);
}

// dwRefPulldown_FreeImages — delete via each image's vtbl slot 0.
void dwRefPulldown::FreeImages()
{
    // Binary calls vtbl slot 0 with the scalar-deleting flag (1) and `this` in
    // ECX, i.e. a virtual `delete`.
    if (this->pImageNormal != NULL)
    {
        delete (dwImage*)this->pImageNormal;
        this->pImageNormal = NULL;
    }
    if (this->pImagePressed != NULL)
    {
        delete (dwImage*)this->pImagePressed;
        this->pImagePressed = NULL;
    }
}

// dwRefPulldown_OnMessage — no-op.
int dwRefPulldown::OnMessage(dwWidgetMsg* pMsg)
{
    (void)pMsg;
    return 0;
}

// dwRefPulldown_ApplyBounds — restore the collapsed rect from the
// saved snapshot unless it was never captured (degenerate).
void dwRefPulldown::ApplyBounds()
{
    if (this->field_0x98 != this->field_0x94 && this->field_0x9a != this->field_0x96)
    {
        this->left = this->field_0x94;
        this->top = this->field_0x96;
        this->right = this->field_0x98;
        this->bottom = this->field_0x9a;
    }
}

// dwRefPulldown_MeasureWidth — widest visible item label (+4) or the
// collapsed rect width; caches into `width`.
int16_t dwRefPulldown::MeasureWidth()
{
    if (this->bIsSubmenu == 0)
        this->width = (int16_t)(this->right - this->left); // rect.right(+0xa) - rect.left(+6)
    else
        this->width = 0;

    dwListNode* pSent = this->pItems.pSentinel;
    for (dwListNode* pNode = pSent->pNext; pNode != pSent; pNode = pNode->pNext)
    {
        dwRefMenuItem* pItem = (dwRefMenuItem*)pNode->pData;
        int len = (int)pItem->name.length; // binary: item+0xc / item+0x14 — the shown name
        if (len != 0 && this->pFontNormal != NULL)
        {
            int16_t w = (int16_t)dwFont_MeasureString(this->pFontNormal, pItem->name.pBuffer, len);
            if ((uint16_t)this->width < (uint16_t)(w + 4))
                this->width = (int16_t)(w + 4);
        }
    }
    return this->width;
}

// dwRefPulldown_ComputeRect — lay out the expanded rectangle.
void dwRefPulldown::ComputeRect()
{
    if (this->bIsSubmenu == 0)
    {
        this->MeasureWidth();
        int16_t l = this->left;
        int16_t top1 = (int16_t)(this->bottom - 1);
        int16_t r = (int16_t)(this->width + l + 4);
        int16_t b = (int16_t)(this->expandHeight + this->bottom);
        this->rectExpandLeft = l;
        this->rectExpandTop = top1;
        this->field_0x94 = l;
        this->field_0x96 = top1;
        this->left = l;
        this->top = top1;
        this->rectExpandRight = r;
        this->rectExpandBottom = b;
        this->field_0x98 = r;
        this->field_0x9a = b;
        this->right = r;
        this->bottom = b;
    }
    else
    {
        this->rectExpandLeft = this->left;
        this->rectExpandTop = this->top;
        this->rectExpandRight = this->right;
        this->rectExpandBottom = (int16_t)(this->expandHeight + this->top);
    }
}

// dwRefPulldown_LayoutExpanded — position the expanded list / a
// hovered submenu, flipping the anchor when it would run off-screen.
void dwRefPulldown::LayoutExpanded(int16_t l, int16_t t, int16_t r, int16_t b, int hoverRow)
{
    // The binary tests against the video mode (always 640x480 for DW); the mode here is
    // window-sized, so use the GUI space's own width.
    int16_t screenW = 640;

    this->bLayingOut = 1;
    if (this->bIsSubmenu == 0)
    {
        dwRefMenuItem* pSel = (dwRefMenuItem*)this->pSelectedItem;
        dwRefPulldown* pSub = pSel ? pSel->pSubmenu : NULL;
        int16_t lineH = (int16_t)(this->pFontNormal ? this->pFontNormal->pHeader->lineHeight : 0);
        int16_t rowY = (int16_t)(this->rectExpandTop + (hoverRow != 0 ? (int16_t)hoverRow * lineH : 0));
        this->field_0x9e = rowY;
        // bottom = row + (submenu item count) * lineH + 1
        int16_t subRows = pSub ? (int16_t)pSub->field_0x18 : 0;
        this->field_0xa2 = (int16_t)(rowY + subRows * lineH + 1);
        this->field_0x9c = (int16_t)(this->rectExpandRight + 1);
        int16_t subW = pSub ? pSub->MeasureWidth() : 0;
        this->field_0xa0 = (int16_t)(this->field_0x9c + subW + 2);
        if (this->field_0xa0 > screenW && this->field_0x9c > 0)
        {
            // Off the right edge: open it to the LEFT of the list instead.
            this->bLayingOut = 0;
            this->field_0xa0 = (int16_t)(this->rectExpandLeft - 1);
            this->field_0x9c = (int16_t)(this->field_0xa0 - (pSub ? pSub->MeasureWidth() : 0) - 2);
        }
    }
    else
    {
        this->rectExpandLeft = l;
        this->rectExpandTop = t;
        this->rectExpandRight = r;
        this->rectExpandBottom = b;
    }
}

void dwRefPulldown::OpenSubmenuAt(dwRefPulldown* pSub, int row, int16_t x, int16_t y)
{
    pSub->pRootMenu = this->pRootMenu;
    this->LayoutExpanded(this->left, this->top, this->right, this->bottom, row);
    pSub->left = this->field_0x9c;  pSub->top = this->field_0x9e;
    pSub->right = this->field_0xa0; pSub->bottom = this->field_0xa2;
    pSub->LayoutExpanded(this->field_0x9c, this->field_0x9e, this->field_0xa0, this->field_0xa2, 0);
    pSub->Invalidate();
    // Save the list rect, then grow this widget's rect to cover the submenu so the
    // mouse keeps reaching this menu while it is over the submenu (binary: the union
    // is stored back into the widget rect; ApplyBounds restores the saved one later).
    this->field_0x94 = this->left; this->field_0x96 = this->top;
    this->field_0x98 = this->right; this->field_0x9a = this->bottom;
    dwRect u;
    u.left = this->left; u.top = this->top; u.right = this->right; u.bottom = this->bottom;
    dwRect_Union(&u, pSub->GetRectPtr());
    this->left = u.left; this->top = u.top; this->right = u.right; this->bottom = u.bottom;
    pSub->OnMouseMove(x, y);
}

// dwRefPulldown_BuildSubmenu — spawn a nested submenu inheriting the
// parent's fonts/colors/wavs; returns it (owned by the item that requested it).
dwRefPulldown* dwRefPulldown::BuildSubmenu(char* pTopicFile)
{
    dwRect zero; zero.left = 0; zero.top = 0; zero.right = 0; zero.bottom = 0;
    // Args mirror the binary dwRefPulldown_BuildSubmenu: fonts/colors/image names/wavs inherited
    // from the parent (image names = field_0xb0/0xbc.pBuffer; wav slots below),
    // pLabel = the topic file, textColor 0, no hot font, bIsSubmenu = 1.
    return new dwRefPulldown(&zero, this->field_0xa4.pBuffer, this->field_0x1c, this->field_0x1e,
                             this->field_0x4c, this->field_0xb0.pBuffer, this->field_0xbc.pBuffer,
                             this->field_0xec.pBuffer, this->field_0xf8.pBuffer,
                             this->field_0xe0.pBuffer, this->field_0xd4.pBuffer,
                             pTopicFile, 0, NULL, 1);
}

// dwRefPulldown_AddItem — insert a new item sorted by its category
// (sortKey, case-insensitive), then reset the visible window to the top.
void dwRefPulldown::AddItem(char* pTopicFile, char* pLabel, char* pSortKey, uint32_t labelColor,
                            uint8_t flag, dwRefPulldown* pSubmenu)
{
    // Binary: new(0x30){ +0x00 = pTopicFile, +0x0c = pLabel (name), +0x18 = pSortKey (category) }
    dwRefMenuItem* pItem = new dwRefMenuItem(pTopicFile, pLabel, pSortKey, (int)labelColor,
                                             flag, pSubmenu);

    dwListNode* pSent = this->pItems.pSentinel;
    dwListNode* pNode = pSent->pNext;
    while (pNode != pSent)
    {
        dwRefMenuItem* pOther = (dwRefMenuItem*)pNode->pData;
        // Sorted by NAME (binary compares item+0x14 with the other item's +0x14).
        if (pItem->name.pBuffer == NULL || pOther->name.pBuffer == NULL)
            break;
        if (dwString_CompareI(pItem->name.pBuffer, pOther->name.pBuffer) < 1)
            break;
        pNode = pNode->pNext;
    }
    this->pItems.InsertAfter(pNode->pPrev, pItem);

    this->field_0x18++;                 // param_1_00[6]++ (item count @0x18)
    this->field_0x44 = 0;               // param_1_00[0x11] = 0 (@0x44)
    this->pVisibleHead = pSent->pNext;  // param_1_00[0x18] = list head (@0x60)
    // param_1_00[0x10] = head->pData (first item) -> pSelectedItem (@0x40)
    this->pSelectedItem = (this->pVisibleHead != pSent) ? this->pVisibleHead->pData : NULL;
    this->Invalidate();
}

// dwRefPulldown_BuildItems — read the TPC (root) or SUB (submenu)
// topic files, add every entry matching this menu's category, growing the
// expand height by one line each.
void dwRefPulldown::BuildItems()
{
    const char* pExt = (this->bIsSubmenu == 0) ? "TPC" : "SUB";
    dwList* pList = (this->bIsSubmenu == 0) ? &this->pListB : &this->pListC;
    inits_EnumFilesByExt(pExt, pList);

    dwListNode* pSent = pList->pSentinel;
    for (dwListNode* pNode = pSent->pNext; pNode != pSent; pNode = pNode->pNext)
    {
        dwConfFile conf;
        const char* pPath = ((dwString*)pNode->pData)->pBuffer;
        dwConfFile_Open(&conf, pPath);
        while (conf.bEof == 0)
        {
            dwConfFile_ReadLine(&conf);
            char* pTok = dwConfFile_NextToken(&conf);
            if (dwString_Equals(pTok, "TOPIC_NAME"))
            {
                this->topicScratch.Assign(conf.pCursor, 0);
                continue;
            }
            if (!dwString_Equals(pTok, "TOPIC_CATEGORY"))
                continue;

            char* pCat = dwConfFile_NextToken(&conf);
            uint32_t hasSubmenu = 0;
            dwConfFile_ParseULong(&conf, &hasSubmenu);
            // Binary: compares [this+0x5c] = label.pBuffer — the menu's category name from
            // the .ifc ("Machines", "Forces", ...). (+0xa4 is the font name; comparing that
            // matched nothing, so every menu stayed empty.)
            if (!dwString_Equals(this->label.pBuffer, pCat))
                continue; // keep scanning the same file

            dwRefPulldown* pSub = NULL;
            if (hasSubmenu != 0)
            {
                pSub = this->BuildSubmenu(this->topicScratch.pBuffer);
                if (pSub != NULL)
                    pSub->pRootMenu = (void*)this;
            }
            this->AddItem((char*)pPath, this->topicScratch.pBuffer, pCat, this->field_0x4c, 1, pSub);
            if (this->pFontNormal != NULL)
                this->expandHeight = (int16_t)(this->expandHeight + (int16_t)this->pFontNormal->pHeader->lineHeight);
            if (this->bIsSubmenu == 0)
                break; // TPC files contribute at most one matching entry
        }
        dwConfFile_Close(&conf);
    }
}

// dwRefPulldown_OnMouseMove — the collapsed/expanded hover state
// machine (open on hover, track the highlighted row, hand off to a submenu).
// Faithful translation over the named struct fields.
int dwRefPulldown::OnMouseMove(int16_t x, int16_t y)
{
    if (this->bIsSubmenu == 0)
    {
        // Binary: the collapsed test is against the SAVED header rect (+0x8c) and the
        // expanded test against the list rect (+0x84) — never the widget rect, which grows
        // to cover an open submenu (testing it took submenu hovers for list rows).
        dwRect headerRect;
        headerRect.left = this->savedLeft; headerRect.top = this->savedTop;
        headerRect.right = this->savedRight; headerRect.bottom = this->savedBottom;
        dwRect listRect;
        listRect.left = this->rectExpandLeft; listRect.top = this->rectExpandTop;
        listRect.right = this->rectExpandRight; listRect.bottom = this->rectExpandBottom;
        if (this->bExpanded == 0 && this->bOpen == 0)
        {
            if (dwRect_ContainsPoint(&headerRect, x, y) != 0)
            {
                if (this->field_0xd4.length != 0 && this->field_0xcc == 0)
                {
                    this->field_0xcc = 1;
                    dwSound_Play(this->field_0xd4.pBuffer);
                }
                this->left = this->savedLeft; this->top = this->savedTop;
                this->right = this->savedRight; this->bottom = this->savedBottom;
                this->Invalidate();
                dwWidget_pMouseTarget = this;
                return 1;
            }
            this->field_0xcc = 0;
            this->Invalidate();
            if (dwWidget_pMouseTarget == this)
            {
                dwWidget_pMouseTarget = NULL;
                return 1;
            }
            return 1;
        }
        else
        {
            if (dwRect_ContainsPoint(&listRect, x, y) != 0)
            {
                this->field_0xcc = 1;
                int lineH = this->pFontNormal ? (int)this->pFontNormal->pHeader->lineHeight : 1;
                int row = (int)((y - this->rectExpandTop) / (lineH ? lineH : 1));
                dwListNode* pNode = this->pVisibleHead ? this->pVisibleHead : this->pItems.pSentinel->pNext;
                for (int i = row; i != 0 && pNode != this->pItems.pSentinel; i--)
                    pNode = pNode->pNext;
                if (pNode == this->pItems.pSentinel)
                    pNode = this->pItems.pSentinel->pNext;
                // Note: guard added — an empty list leaves pNode at the sentinel (crashed on
                // the first mouse move over an open, empty menu).
                dwRefMenuItem* pItem = (pNode != this->pItems.pSentinel) ? (dwRefMenuItem*)pNode->pData : NULL;
                this->pSelectedItem = pItem;
                if (pItem && pItem->pSubmenu != NULL)
                    this->OpenSubmenuAt(pItem->pSubmenu, row, x, y);
                this->field_0x1d = 1;
                this->Invalidate();
                return 1;
            }
            // outside the collapsed header
            if (!(x < this->savedLeft || this->savedRight <= x ||
                  y < this->savedTop || this->savedBottom <= y))
            {
                this->pSelectedItem = (this->pItems.pSentinel->pNext != this->pItems.pSentinel)
                    ? this->pItems.pSentinel->pNext->pData : NULL; // field_0x60->pData (guarded)
                this->field_0x1d = 1;
                this->Invalidate();
                dwWidget_pMouseTarget = this;
                return 1;
            }
            dwRefMenuItem* pSel = (dwRefMenuItem*)this->pSelectedItem;
            if (pSel && pSel->pSubmenu != NULL)
            {
                dwPoint pt; pt.x = x; pt.y = y;
                if (pSel->pSubmenu->ContainsPoint(&pt) != 0)
                {
                    dwWidget_pMouseTarget = pSel->pSubmenu;
                    pSel->pSubmenu->OnMouseMove(x, y);
                    return 1;
                }
            }
            if (y < this->savedBottom)
            {
                this->field_0xcc = 0;
                this->bOpen = 0;
                this->bExpanded = 0;
                if (dwWidget_pMouseTarget == this) dwWidget_pMouseTarget = NULL;
                this->Invalidate();
                this->left = this->savedLeft; this->top = this->savedTop;
                this->right = this->savedRight; this->bottom = this->savedBottom;
                this->Invalidate();
                return 1;
            }
        }
    }
    else
    {
        this->field_0xcc = 0;
        if (this->bExpanded == 0 && this->bOpen == 0)
        {
            this->bExpanded = 1; this->bOpen = 1;
            dwWidget_pMouseTarget = this;
            this->ComputeRect();
            this->Invalidate();
        }
        dwRect expand; expand.left = this->rectExpandLeft; expand.top = this->rectExpandTop;
        expand.right = this->rectExpandRight; expand.bottom = this->rectExpandBottom;
        int inside = !(x < this->rectExpandLeft || this->rectExpandRight <= x ||
                       y < this->rectExpandTop || this->rectExpandBottom <= y);
        if (inside && this->bExpanded != 0 && this->bOpen != 0)
        {
            this->field_0xcc = 0;
            int lineH = this->pFontNormal ? (int)this->pFontNormal->pHeader->lineHeight : 1;
            int row = (int)((y - this->rectExpandTop) / (lineH ? lineH : 1));
            dwListNode* pNode = this->pVisibleHead ? this->pVisibleHead : this->pItems.pSentinel->pNext;
            for (int i = row; i != 0 && pNode != this->pItems.pSentinel; i--)
                pNode = pNode->pNext;
            if (pNode == this->pItems.pSentinel)
            {
                // Past the end: the LAST item (binary: sentinel->pPrev->pData).
                this->pSelectedItem = (this->pItems.pSentinel->pPrev != this->pItems.pSentinel)
                    ? this->pItems.pSentinel->pPrev->pData : NULL;
                this->field_0x1d = 1;
                this->Invalidate();
                return 1;
            }
            this->pSelectedItem = pNode->pData;
            this->field_0x1d = 1;
            this->Invalidate();
            dwRefMenuItem* pItem = (dwRefMenuItem*)this->pSelectedItem;
            if (pItem && pItem->pSubmenu != NULL)
            {
                this->OpenSubmenuAt(pItem->pSubmenu, row, x, y); // (nested menus: never in the data)
                return 1;
            }
        }
        else if (this->bExpanded != 0 && this->bOpen != 0)
        {
            dwRefPulldown* pRoot = (dwRefPulldown*)this->pRootMenu;
            if (pRoot != NULL)
            {
                int inRootList = !(x < pRoot->rectExpandLeft || pRoot->rectExpandRight <= x ||
                                   y < pRoot->rectExpandTop || pRoot->rectExpandBottom <= y);
                if (inRootList)
                {
                    // Back over the root's list: the root takes the mouse again; moving to
                    // a different row closes this submenu and restores the root's rect.
                    int lineH = this->pFontNormal ? (int)this->pFontNormal->pHeader->lineHeight : 1;
                    int row = (int)((y - this->rectExpandTop) / (lineH ? lineH : 1));
                    dwWidget_pMouseTarget = pRoot;
                    if (row != (int)this->field_0xca)
                    {
                        this->field_0xcc = 0; this->bOpen = 0; this->bExpanded = 0;
                        if (dwWidget_pMouseTarget == pRoot) dwWidget_pMouseTarget = NULL;
                        this->Invalidate();
                        pRoot->ApplyBounds();
                        pRoot->Invalidate();
                    }
                    this->field_0xca = (int16_t)row;
                    return 1;
                }
                if (y < pRoot->rectExpandTop && y < this->rectExpandTop)
                {
                    this->field_0xcc = 0; this->bOpen = 0; this->bExpanded = 0;
                    if (dwWidget_pMouseTarget == this) dwWidget_pMouseTarget = NULL;
                    this->Invalidate();
                    pRoot->ApplyBounds();
                    pRoot->OnMouseMove(x, y);
                }
            }
        }
    }
    return 1;
}

// dwRefPulldown_OnMouseDown — open on the header, select the hovered
// item (dropping into a submenu or closing), or dismiss on an outside click.
int dwRefPulldown::OnMouseDown(int16_t x, int16_t y)
{
    // Binary: both header tests use the SAVED (collapsed) rect at +0x8c, not the current
    // rect — once expanded the current rect covers the whole list, and testing it here
    // swallowed every item click as a "click on the header: collapse".
    dwRect headerRect;
    headerRect.left = this->savedLeft; headerRect.top = this->savedTop;
    headerRect.right = this->savedRight; headerRect.bottom = this->savedBottom;
    if (this->bExpanded == 0 && this->bOpen == 0 &&
        dwRect_ContainsPoint(&headerRect, x, y) != 0)
    {
        this->bExpanded = 1; this->bOpen = 1;
        dwWidget_pMouseTarget = this;
        this->ComputeRect();
        if (this->field_0xd4.length != 0)
            dwSound_Play(this->field_0xe0.pBuffer);
        this->Invalidate();
        return 1;
    }
    if (dwRect_ContainsPoint(&headerRect, x, y) != 0 &&
        this->bExpanded != 0 && this->bOpen != 0)
    {
        this->bExpanded = 0; this->bOpen = 0;
        if (dwWidget_pMouseTarget == this) dwWidget_pMouseTarget = NULL;
        this->Invalidate();
        this->left = this->savedLeft; this->top = this->savedTop;
        this->right = this->savedRight; this->bottom = this->savedBottom;
        return 1;
    }
    if (this->bExpanded == 0)
        return 0;

    int inside = !(x < this->rectExpandLeft || this->rectExpandRight <= x ||
                   y < this->rectExpandTop || this->rectExpandBottom <= y);
    if (this->bOpen != 0 && inside)
    {
        if (this->bIsSubmenu != 0)
        {
            dwRefPulldown* pRoot = (dwRefPulldown*)this->pRootMenu;
            pRoot->field_0xcc = 0; pRoot->bOpen = 0; pRoot->bExpanded = 0;
            this->bExpanded = 0; this->bOpen = 0;
            if (this->field_0xec.length != 0)
                dwSound_Play(this->field_0xf8.pBuffer);
            // Binary: post { field_0x4c, item->topicFile } — the topic opens.
            {
                dwRefMenuItem* pSel = (dwRefMenuItem*)this->pSelectedItem;
                if (pSel != NULL)
                {
                    dwWidgetMsg msg;
                    msg.code = (int32_t)this->field_0x4c;
                    msg.pSender = pSel->topicFile.pBuffer;
                    msg.param = 0;
                    msg.pTarget = NULL;
                    dwWidget_DispatchMsg(&msg, NULL);
                }
            }
            if (dwWidget_pMouseTarget == this) dwWidget_pMouseTarget = NULL;
            pRoot->left = pRoot->savedLeft; pRoot->top = pRoot->savedTop;
            pRoot->right = pRoot->savedRight; pRoot->bottom = pRoot->savedBottom;
            pRoot->Invalidate();
            return 1;
        }
        if (this->field_0xec.length != 0)
            dwSound_Play(this->field_0xec.pBuffer);
        dwRefMenuItem* pSel = (dwRefMenuItem*)this->pSelectedItem;
        if (pSel && pSel->pSubmenu != NULL)
            return 1;
        this->field_0xcc = 0; this->bExpanded = 0; this->bOpen = 0;
        this->Invalidate();
        this->left = this->savedLeft; this->top = this->savedTop;
        this->right = this->savedRight; this->bottom = this->savedBottom;
        // Binary: post { field_0x4c, item->topicFile } (the .ifc's Msg column, e.g. 7004 =
        // 0x1b5c "show topic") — this was missing, so picking a topic did nothing.
        if (pSel != NULL)
        {
            dwWidgetMsg msg;
            msg.code = (int32_t)this->field_0x4c;
            msg.pSender = pSel->topicFile.pBuffer;
            msg.param = 0;
            msg.pTarget = NULL;
            dwWidget_DispatchMsg(&msg, NULL);
        }
        if (dwWidget_pMouseTarget == this) dwWidget_pMouseTarget = NULL;
        return 1;
    }
    if (this->bExpanded != 0 && this->bOpen != 0 && !inside)
    {
        if (this->bIsSubmenu != 0)
        {
            if (this->pRootMenu != NULL)
            {
                this->bExpanded = 0; this->bOpen = 0;
                this->Invalidate();
                dwWidget_pMouseTarget = (dwWidget*)this->pRootMenu;
                ((dwRefPulldown*)this->pRootMenu)->OnMouseDown(x, y);
            }
            this->Invalidate();
            return 1;
        }
        this->field_0xcc = 0; this->bExpanded = 0; this->bOpen = 0;
        this->Invalidate();
        if (dwWidget_pMouseTarget == this) dwWidget_pMouseTarget = NULL;
        this->left = this->savedLeft; this->top = this->savedTop;
        this->right = this->savedRight; this->bottom = this->savedBottom;
        this->Invalidate();
        return 1;
    }
    return 0;
}

// dwRefPulldown_Draw — paint the collapsed image/label, and when
// open the scrollable expanded item list (highlight + shadowed text). Faithful
// over the named fields; the two open-state branches (submenu vs root) are
// merged since they paint identically.
void dwRefPulldown::Draw(dwImageBits* pDestBits, dwRect* pClipRect)
{
    this->EnsureImages();

    if (this->bEnabled != 0)
    {
        if (this->field_0xcc == 0 && this->bOpen == 0)
        {
            if (this->pImageNormal != NULL)
            {
                // Note: binary calls image vtbl +0x04 = Blit via a raw slot read;
                // 64-bit slots are 8 bytes (crashed) — use the virtual call.
                ((dwImage*)this->pImageNormal)->Blit(pDestBits, this->savedLeft, this->savedTop, pClipRect);
            }
        }
        else if (this->bIsSubmenu == 0)
        {
            if (this->pImagePressed != NULL)
            {
                // Note: raw vtbl +0x04 slot call in the binary (see above).
                ((dwImage*)this->pImagePressed)->Blit(pDestBits, this->savedLeft, this->savedTop, pClipRect);
            }
            // collapsed label (centered, shadowed color field_0xcd)
            dwFontHeader* pHdr = this->pFontHot ? this->pFontHot->pHeader : NULL;
            if (pHdr != NULL)
            {
                int centerY = this->savedTop +
                    (int16_t)(((this->savedBottom - this->savedTop) - (int)pHdr->lineHeight) / 2) + // binary: bottom - top
                    (int16_t)pHdr->bpp;
                int16_t w = (int16_t)dwFont_MeasureString(this->pFontHot, this->label.pBuffer,
                                                          (int)this->label.length);
                int16_t x = (int16_t)(this->savedLeft + ((this->savedRight - this->savedLeft) - w) / 2);
                dwPoint pos; pos.x = x; pos.y = (int16_t)centerY;
                dwFont_DrawStringClipped(pDestBits, this->pFontHot, &pos, this->label.pBuffer,
                                         this->field_0xcd, pClipRect);
            }
        }
    }

    // Expanded item list.
    if (this->pItems.pSentinel == this->pItems.pSentinel->pNext) return; // empty
    if (this->bExpanded == 0) return;
    if (this->bOpen == 0) return;

    dwRect box;
    box.left = this->rectExpandLeft; box.top = this->rectExpandTop;
    box.right = this->rectExpandRight; box.bottom = this->rectExpandBottom;
    dwRect boxIn;
    boxIn.left = (int16_t)(box.left + 1); boxIn.top = (int16_t)(box.top + 1);
    boxIn.right = (int16_t)(box.right + 1); boxIn.bottom = (int16_t)(box.bottom + 1);
    dwImageDraw_BlendRect(pDestBits, &box, 0xdc, pClipRect);
    dwImageDraw_FrameRect(pDestBits, &box, 0x1f, pClipRect);
    dwImageDraw_FrameRect(pDestBits, &boxIn, dwColormap_transparentIdx, pClipRect);

    int16_t lineH = (int16_t)(this->pFontNormal ? this->pFontNormal->pHeader->lineHeight : 0);
    int16_t rowY = this->rectExpandTop; // sStack_16 seed = box.top
    dwListNode* pNode = this->pVisibleHead ? this->pVisibleHead : this->pItems.pSentinel->pNext;
    while (pNode != this->pItems.pSentinel)
    {
        if (pClipRect->bottom <= rowY) return;

        dwRefMenuItem* pItem = (dwRefMenuItem*)pNode->pData;
        if (this->field_0x1d != 0 && (void*)pItem == this->pSelectedItem)
        {
            if (pItem->pSubmenu != NULL && this->bIsSubmenu == 0)
                pItem->pSubmenu->DrawChild(pDestBits, pClipRect); // binary: ecx = the submenu
                                                                   // (drawing THIS recursed until the stack overflowed)
            dwRect hi; hi.left = box.left; hi.top = rowY;
            hi.right = box.right; hi.bottom = (int16_t)(rowY + lineH);
            dwImageDraw_BlendRect(pDestBits, &hi, 0xdc, pClipRect);
            dwImageDraw_FrameRect(pDestBits, &hi, 0x1f, pClipRect);
        }

        // shadowed + normal text passes
        int16_t textX = (int16_t)(box.left + 4);
        int16_t glyphTop = (int16_t)((this->pFontNormal ? (int16_t)this->pFontNormal->pHeader->bpp : 0) + 1 + rowY);
        dwPoint p1; p1.x = textX; p1.y = glyphTop;
        dwFont_DrawStringClipped(pDestBits, this->pFontNormal, &p1, pItem->name.pBuffer,
                                 (uint8_t)dwColormap_transparentIdx, pClipRect);
        dwPoint p2; p2.x = (int16_t)(box.left + 3);
        p2.y = (int16_t)((this->pFontNormal ? (int16_t)this->pFontNormal->pHeader->bpp : 0) + rowY);
        dwFont_DrawStringClipped(pDestBits, this->pFontNormal, &p2, pItem->name.pBuffer,
                                 this->field_0x1c, pClipRect);

        pNode = pNode->pNext;
        rowY = (int16_t)(rowY + lineH);
    }
}
