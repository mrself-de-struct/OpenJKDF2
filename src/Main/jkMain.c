#include "jkMain.h"

#include "../jk.h"
#include "Engine/rdroid.h"
#include "Main/sithMain.h"
#include "Devices/sithControl.h"
#include "Devices/sithSoundMixer.h"
#include "Dss/sithGamesave.h"
#include "Engine/sithCamera.h"
#include "Dss/sithMulti.h"
#include "Engine/sithRender.h"
#include "Engine/sithCamera.h"
#include "Gameplay/sithTime.h"
#include "Main/jkSmack.h"
#include "Main/jkGame.h"
#include "Main/jkCredits.h"
#include "Main/jkCutscene.h"
#include "Main/jkHudInv.h"
#include "Main/jkHud.h"
#include "Main/jkHudScope.h"
#include "Main/jkHudCameraView.h"
#include "Main/jkDev.h"
#include "Main/jkEpisode.h"
#include "Main/jkRes.h"
#include "Main/jkStrings.h"
#include "Gui/jkGUIRend.h"
#include "Gui/jkGUI.h"
#include "Gui/jkGUIMultiTally.h"
#include "Gui/jkGUIForce.h"
#include "Gui/jkGUIMain.h"
#include "Gui/jkGUITitle.h"
#include "Gui/jkGUIDialog.h"
#include "Gui/jkGUIEsc.h"
#include "Gui/jkGUISingleTally.h"
#include "Gui/jkGUIMultiplayer.h"
#include "Gui/jkGUIDisplay.h"
#include "World/jkPlayer.h"
#include "Dw/dwMain.h"
#include "Gameplay/jkSaber.h"
#include "World/sithWorld.h"
#include "Platform/stdControl.h"
#include "Win95/Windows.h"
#include "Win95/Video.h"
#include "Win95/stdComm.h"
#include "Main/Main.h" // Added: Main_NextRotationMap
#include "Win95/stdGob.h" // Added: jkMain_FindLevel
#include "General/stdFileUtil.h"
#include "General/stdFnames.h"
#if defined(QOL_IMPROVEMENTS) && defined(PLATFORM_DPLAY)
static void jkMain_FollowHostMap(void); // Added: map rotation
static char jkMain_aMissingMap[128];  // Added: joiner: the host went on to a level we don't have
#endif
#include "Win95/stdDisplay.h"
#include "Win95/Window.h"
#include "General/util.h"
#include "General/stdBitmap.h"
#include "General/stdPalEffects.h"
#include "General/stdString.h"
#include "World/jkPlayer.h"
#include "Dss/jkDSS.h"
#include "stdPlatform.h"

#if defined(TARGET_RETRO_HOMEBREW)
#define TICKRATE_MS (0) // no cap
#elif defined(QOL_IMPROVEMENTS)
#define TICKRATE_MS (jkPlayer_fpslimit ? 1000 / jkPlayer_fpslimit : 0) // no cap
#else
#define TICKRATE_MS (20) // 50fps
#endif

char jkMain_aLevelJklFnameMots[128];
char jkMain_motsIdk[128];

jkEpisodeEntry* jkMain_pEpisodeEnt = NULL;
jkEpisodeEntry* jkMain_pEpisodeEnt2 = NULL;

static jkGuiStateFuncs jkMain_aGuiStateFuncs[16] = {
    {0,  0,  0},
    {jkMain_VideoShow, jkMain_VideoTick, jkMain_VideoLeave},
    {jkMain_TitleShow, jkMain_TitleTick, jkMain_TitleLeave},
    {jkMain_MainShow, jkMain_MainTick, jkMain_MainLeave},
    {jkMain_VideoShow, jkMain_VideoTick, jkMain_VideoLeave},
    {jkMain_GameplayShow, jkMain_GameplayTick, jkMain_GameplayLeave},
    {jkMain_EscapeMenuShow, jkMain_EscapeMenuTick, jkMain_EscapeMenuLeave},
    {jkMain_CdSwitchShow,  0,  0},
    {jkMain_VideoShow, jkMain_VideoTick, jkMain_VideoLeave},
    {jkMain_EndLevelScreenShow, jkMain_EndLevelScreenTick, jkMain_EndLevelScreenLeave},
    {jkMain_VideoShow, jkMain_VideoTick, jkMain_VideoLeave},
    {jkMain_ChoiceShow, jkMain_ChoiceTick, jkMain_ChoiceLeave},
    {jkMain_CutsceneShow, jkMain_CutsceneTick, jkMain_CutsceneLeave},
    {jkMain_CreditsShow, jkMain_CreditsTick, jkMain_CreditsLeave},
    {jkMain_UnkShow, jkMain_UnkTick, jkMain_UnkLeave},
    {jkMain_VideoShow, jkMain_VideoTick, jkMain_VideoLeave}, // MOTS added
};

void jkMain_Startup()
{
    jkPlayer_Startup();
    jkPlayer_InitForceBins();
    jkMain_bInit = 1;
}

void jkMain_Shutdown()
{
    jkPlayer_Shutdown();
    sithClose();

    // Added: memleak
    if ( jkEpisode_mLoad.paEntries )
    {
        JK_FREE(jkEpisode_mLoad.paEntries);
        jkEpisode_mLoad.paEntries = 0;
    }

    // Added: prevent UAF
    jkMain_pEpisodeEnt = NULL;
    jkMain_pEpisodeEnt2 = NULL;

    jkMain_bInit = 0;
}

// TODO merge SDL2 in
#if !defined(SDL2_RENDER) && !defined(TARGET_RETRO_HOMEBREW)
int jkMain_SetVideoMode()
{
    signed int result; // eax
    char16_t *v1; // eax
    char16_t *v2; // eax
    char16_t *v3; // [esp-4h] [ebp-10h]
    char16_t *v4; // [esp-4h] [ebp-10h]

    if ( jkGame_isDDraw )
        return 0;
    jkPlayer_Open();
    if ( Video_SetVideoDesc(sithWorld_g_pCurrentWorld->colormaps->colors) )
        goto LABEL_12;
    if ( !sithNet_isMulti )
    {
        jkMain_bInModalGui = 1;
        sithControl_Close();
        v3 = jkStrings_GetUniStringWithFallback("ERR_CHANGING_VIDEO_DESC");
        v1 = jkStrings_GetUniStringWithFallback("ERR_CHANGING_VIDEO_MODE");
        jkGuiDialog_ErrorDialog(v1, v3);
        sithControl_Open();
        jkMain_bInModalGui = 0;
    }
    _memcpy(&Video_modeStruct, &Video_modeStruct2, sizeof(Video_modeStruct));
    jkGuiDisplay_sub_4149C0();
    if ( Video_SetVideoDesc(sithWorld_g_pCurrentWorld->colormaps->colors) )
    {
LABEL_12:
        Windows_InitGdi(stdDisplay_pCurDevice->video_device[0].windowedMaybe);
        jkGame_isDDraw = 1;
        result = 1;
    }
    else
    {
        jkPlayer_Close();
        if ( sithControl_IsOpen() )
            sithControl_Close();
        if ( jkGuiRend_menuNestDepth )
            jkGuiRend_bStateChangePending = 1;
        jkSmack_stopTick = 1;
        jkSmack_nextGuiState = 3;
        v4 = jkStrings_GetUniStringWithFallback("ERR_CHANGING_VIDEO_ABORT");
        v2 = jkStrings_GetUniStringWithFallback("ERR_CHANGING_VIDEO_MODE");
        jkGuiDialog_ErrorDialog(v2, v4);
        result = 0;
    }
    return result;
}
#endif

void jkMain_SetVideoModeGdi()
{
    if ( jkGame_isDDraw )
    {
        Windows_ShutdownGdi();
        Video_SwitchToGDI();
        jkPlayer_Close();
        jkGame_isDDraw = 0;
    }
}

void jkMain_InitPlayerThings()
{
    jkPlayer_InitThings();
}

int jkMain_SwitchTo5_2()
{
    signed int result; // eax

    result = 1;
    jkSmack_gameMode = 4;
    jkPlayer_bLoadingSomething = 1;
    if ( jkGuiRend_menuNestDepth )
        jkGuiRend_bStateChangePending = 1;
    jkSmack_stopTick = 1;
    jkSmack_nextGuiState = 5;
    return result;
}

int jkMain_SwitchTo5(char *pJklFname)
{
    signed int result; // eax

    _strncpy(jkMain_aLevelJklFname, pJklFname, 0x7Fu);
    jkMain_aLevelJklFname[127] = 0;
    jkSmack_gameMode = 3;
    result = 1;
    if ( jkGuiRend_menuNestDepth )
        jkGuiRend_bStateChangePending = 1;
    jkSmack_stopTick = 1;
    jkSmack_nextGuiState = 5;
    return result;
}

// MOTS altered
void jkMain_GuiAdvance()
{
    unsigned int v1; // esi
    int v3; // esi
    int v4; // esi
    void (__cdecl *v5)(int, int); // ecx
    void (__cdecl *v7)(int, int); // ecx
    void (__cdecl *v8)(int); // ecx

    // Added: Droidworks mode replaces the jk app layer wholesale
    if (Main_bDroidWorks) {
        dwMain_GuiAdvance();
        return;
    }

    if ( !g_app_suspended )
    {
        if ( jkMain_bAppResumed )
            stdControl_SetActivation(0);
        if ( jkMain_bLevelActive )
        {
            if ( sithNet_isMulti && !jkMain_bInModalGui)
            {
                v1 = stdPlatform_GetTimeMsec();
                
                if (v1 > jkMain_lastTickMs + TICKRATE_MS)
                {
                    jkMain_lastTickMs = v1;
                    if (!sithUpdate()) return;
                }
                
                if ( g_sithMode == 5 )
                {
                    if ( jkGuiRend_menuNestDepth )
                        jkGuiRend_bStateChangePending = 1;
                    jkSmack_stopTick = 1;
                    jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
                    jkMain_bAppResumed = 0;
                    return;
                }
                if ( sithMulti_bTimelimitMet )
                {
                    sithMulti_bTimelimitMet = 0;
                    if ( sithNet_isServer )
                        jkDSS_SendEndLevel();
                }
                if ( sithMain_bEndLevel )
                {
                    sithMain_bEndLevel = 0;
                    if (Main_bMotsCompat)
                        jkPlayer_idkEndLevel(); // MOTS added
                    jkMain_EndLevel(1);
                }
                jkPlayer_nullsub_1(&playerThings[playerThingIdx]);
                jkGame_dword_552B5C += stdPlatform_GetTimeMsec() - v1;
                v3 = stdPlatform_GetTimeMsec();
                if ( g_app_suspended && jkSmack_currentGuiState != 6 ) {
#if defined(SDL2_RENDER) || defined(TARGET_RETRO_HOMEBREW)
                    if (jkMain_lastTickMs == v1)
#endif
                    jkGame_Update();
                }
                jkGame_updateMsecsTotal += stdPlatform_GetTimeMsec() - v3;
            }
        }
        jkMain_bAppResumed = 0;
        return;
    }

    if ( !jkMain_bAppResumed )
    {
        switch ( jkSmack_currentGuiState )
        {
            case JK_GAMEMODE_VIDEO:
            case JK_GAMEMODE_VIDEO2:
            case JK_GAMEMODE_VIDEO3:
            case JK_GAMEMODE_VIDEO4:
            case JK_GAMEMODE_MOTS_CUTSCENE: // MOTS added
                jkCutscene_PauseShow(0);
                break;
            case JK_GAMEMODE_GAMEPLAY:
                stdControl_SetActivation(1);
                jkGame_ddraw_idk_palettes();
                break;
            default:
                break;
        }
        stdControl_Flush();
        jkMain_bAppResumed = 1;
    }
    if ( jkSmack_stopTick && !jkGuiRend_menuNestDepth )
    {
        jkGuiRend_bStateChangePending = 0;
        v4 = jkSmack_currentGuiState;
        v5 = jkMain_aGuiStateFuncs[jkSmack_currentGuiState].leaveFunc;
        if ( v5 )
            v5(jkSmack_currentGuiState, jkSmack_nextGuiState);
        //jk_printf("leave %u\n", jkSmack_currentGuiState);

        jkSmack_stopTick = 0;
        jkSmack_currentGuiState = jkSmack_nextGuiState;
        v7 = jkMain_aGuiStateFuncs[jkSmack_nextGuiState].showFunc;
        if ( !v7 )
            goto LABEL_35;
        //jk_printf("show %u\n", jkSmack_currentGuiState);
        v7(jkSmack_nextGuiState, v4);
        //jk_printf("showed %u\n", jkSmack_currentGuiState);
    }
LABEL_35:
    if ( !jkSmack_stopTick )
    {
        //jk_printf("tick %u %x\n", jkSmack_currentGuiState, jkMain_aGuiStateFuncs[jkSmack_currentGuiState].tickFunc);
        v8 = jkMain_aGuiStateFuncs[jkSmack_currentGuiState].tickFunc;
        if ( v8 )
            v8(jkSmack_currentGuiState);
    }
}

void jkMain_EscapeMenuShow(int a1, int a2)
{
    if ( !sithNet_isMulti ){
        sithTime_Pause();
    }

    // Added
    stdBitmap_EnsureData(jkGui_stdBitmaps[JKGUI_BM_BK_ESC]);
    
    jkGui_SetModeMenu(jkGui_stdBitmaps[JKGUI_BM_BK_ESC]->palette);
    jkGuiEsc_Show();
}

void jkMain_EscapeMenuTick(int a2)
{
    unsigned int v1; // esi
    int v3; // esi

    if (!sithNet_isMulti) {
        return;
    }

    if (jkMain_bInModalGui) {
        return;
    }
    
    if (!jkMain_bLevelActive) {
        return;
    }

    v1 = stdPlatform_GetTimeMsec();
    
    if (v1 > jkMain_lastTickMs + TICKRATE_MS)
    {
        jkMain_lastTickMs = v1;
        if (sithUpdate()) return;
    }
    
    if ( g_sithMode == 5 )
    {
        if ( jkGuiRend_menuNestDepth )
            jkGuiRend_bStateChangePending = 1;
        jkSmack_stopTick = 1;
        jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
    }
    else
    {
        if ( sithMulti_bTimelimitMet )
        {
            sithMulti_bTimelimitMet = 0;
            if ( sithNet_isServer )
                jkDSS_SendEndLevel();
        }
        if ( sithMain_bEndLevel )
        {
            sithMain_bEndLevel = 0;
            if (Main_bMotsCompat)
                jkPlayer_idkEndLevel(); // MOTS added
            jkMain_EndLevel(1);
        }
        jkPlayer_nullsub_1(&playerThings[playerThingIdx]);
        jkGame_dword_552B5C += stdPlatform_GetTimeMsec() - v1;
        v3 = stdPlatform_GetTimeMsec();
        if ( g_app_suspended && a2 != 6 ) {
#ifdef SDL2_RENDER
        if (jkMain_lastTickMs == v1)
#endif
            jkGame_Update();
        }
        jkGame_updateMsecsTotal += stdPlatform_GetTimeMsec() - v3;
    }
}

// MOTS altered
void jkMain_EscapeMenuLeave(int a2, int a3)
{
    int v3; // eax

    if ( !sithNet_isMulti )
        sithTime_Resume();

    // MOTS added
    if ( a3 != JK_GAMEMODE_GAMEPLAY && a3 != JK_GAMEMODE_MOTS_CUTSCENE)
    {
        if ( a3 == JK_GAMEMODE_ESCAPE )
        {
            stdControl_SetActivation(0);
            sithSoundMixer_StopAll();
        }
        if ( jkGame_isDDraw )
        {
            Windows_ShutdownGdi();
            Video_SwitchToGDI();
            jkPlayer_Close();
            jkGame_isDDraw = 0;
        }
        if ( a3 != JK_GAMEMODE_ESCAPE && jkMain_bInit )
        {
            jkPlayer_Shutdown();
            sithClose();
            jkMain_bInit = 0;
            jkMain_bLevelActive = 0;
        }
        if ( sithNet_isMulti && a3 != JK_GAMEMODE_ESCAPE )
        {
            jkMain_bLevelActive = 0;
            if ( a3 == 3 ) {
                // MOTS added
                if (Main_bMotsCompat) {
                    sithMulti_LobbyMessage();
                }
                sithMulti_Shutdown();
            }
            else {
                sithMulti_LobbyMessage();
            }
            if ( sithNet_isServer )
                DirectPlay_SetSessionFlagidk(0);
            jkMain_bInModalGui = 1;
#ifdef PLATFORM_DPLAY
            // Added: the server asked everyone to reconnect: no scores (which wait for a
            // click once the game is gone), straight on to the menus, which join again
            if ( stdComm_DPlay_RejoinAsked() )
                v3 = -1;
            else
#endif
            v3 = jkGuiMultiTally_Show(sithNet_isMulti);
            jkMain_bInModalGui = 0;
            if ( v3 == -1 )
            {
                sithMulti_Shutdown();
                if ( jkGuiRend_menuNestDepth )
                    jkGuiRend_bStateChangePending = 1;
                jkSmack_stopTick = 1;
                jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
            }
        }
    }
    jkGui_SetModeGame();
}

// MOTS altered
void jkMain_EndLevelScreenShow(int a1, int a2)
{
    stdControl_SetActivation(0); // Added

    if (!Main_bMotsCompat) {
        if ( jkEpisode_mLoad.type != JK_EPISODE_SINGLEPLAYER && jkSmack_gameMode == 2
          || jkGuiSingleTally_Show() != -1
          && (sithPlayer_GetInvItemAmount(SITHBIN_NEW_STARS) <= 0.0 && sithPlayer_GetInvItemAmount(SITHBIN_SPEND_STARS) <= 0.0
           || jkGuiForce_Show(1, 0.0, jkMain_dword_552B98, 0, 0, 1) != -1) )
        {
            jkMain_StartNextLevelInEpisode(0, 1);
            return;
        }
    }
    else 
    { 
        // MOTS added
        if (jkGuiSingleTally_Show() != -1) {
            if (sithPlayer_GetInvItemAmount(SITHBIN_NEW_STARS) <= 0.0 && sithPlayer_GetInvItemAmount(SITHBIN_SPEND_STARS) <= 0.0) {
                jkMain_StartNextLevelInEpisode(0, 1);
                return;
            }

            jkPlayer_idkEndLevel();
            if (jkGuiForce_Show(1, 0.0, jkMain_dword_552B98, 0, 0, 1) != -1) {
                jkMain_StartNextLevelInEpisode(0, 1);
                return;
            }
        }
    }

    if ( jkGuiRend_menuNestDepth )
        jkGuiRend_bStateChangePending = 1;
    jkSmack_stopTick = 1;
    jkSmack_nextGuiState = 3;
    return;
}

void jkMain_EndLevelScreenTick(int a1)
{
    ;
}

void jkMain_EndLevelScreenLeave(int a1, int a2)
{
    ;
}

void jkMain_GameplayShow(int a1, int a2)
{
    signed int level_loaded; // esi
    signed int v3; // eax
    char16_t *v4; // eax
    DWORD v5; // eax
    char16_t *v6; // [esp-4h] [ebp-Ch]

    level_loaded = 0;

    // MOTS added something here TODO
    if (a2 == JK_GAMEMODE_MOTS_CUTSCENE) {
        stdString_SafeStrCopy(jkMain_aLevelJklFname,jkMain_aLevelJklFnameMots, 128);
    }
    else if ( a2 == JK_GAMEMODE_ESCAPE )
    {
        sithSoundMixer_ResumeAll();
        sithSoundMixer_ResumeMusic(1);
#ifdef SDL2_RENDER
        jkGame_isDDraw = 0;
        Window_RemoveMsgHandler(Windows_GdiHandler);
#endif
    }
    else if ( jkSmack_gameMode == JK_GAMEMODE_VIDEO2 )
    {
        jkPlayer_Startup();
        jkPlayer_InitForceBins();
        jkMain_bInit = 1;
        jkPlayer_InitSaber();
        sithOpenPostProcess();
    }
    else {
        // MOTS added
        jkMain_motsIdk[0] = 0;

        if ( jkSmack_gameMode == 1 )
        {
            jkGui_copies_string(gamemode_1_str);
            jkGuiTitle_ShowLoading(gamemode_1_str, 0);
        }
        else
        {
#if defined(QOL_IMPROVEMENTS) && defined(PLATFORM_DPLAY)
            if ( jkSmack_gameMode == 2 )
                jkMain_FollowHostMap(); // Added: a dedicated host's next level
#endif
            jkGui_copies_string(jkMain_aLevelJklFname);
            jkGuiTitle_ShowLoading(jkMain_aLevelJklFname, 0);
        }

        // MOTS added:
        // jkEpisode_Shutdown
        v3 = 0; // Added
        if ( jkSmack_gameMode == 0)
        {
#ifdef JKM_DSS
            jkPlayer_SetAmmoMaximums(0);
#endif
            v3 = sithMain_Mode1Init(jkMain_aLevelJklFname);
        }
        else if ( jkSmack_gameMode == 1 )
        {
#ifdef JKM_DSS
            jkPlayer_SetAmmoMaximums(0);
#endif
            v3 = sithGamesave_Restore(jkMain_aLevelJklFname, 0, 1);
        }
        else if ( jkSmack_gameMode == 2 )
        {
#ifdef JKM_DSS
            jkPlayer_SetAmmoMaximums(jkPlayer_personality);
#endif
            v3 = sithOpenMulti(jkMain_aLevelJklFname);
        }

        level_loaded = v3;
        jkGuiTitle_LoadingFinalize();
        if ( !level_loaded )
        {
            if ( jkGame_isDDraw )
            {
                Windows_ShutdownGdi();
                Video_SwitchToGDI();
                jkPlayer_Close();
                jkGame_isDDraw = 0;
            }
            if ( jkGuiRend_menuNestDepth )
                jkGuiRend_bStateChangePending = 1;
            jkSmack_stopTick = 1;
            jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
            v6 = jkStrings_GetUniStringWithFallback("ERR_CANNOT_LOAD_LEVEL");
            v4 = jkStrings_GetUniStringWithFallback("ERROR");
#if defined(QOL_IMPROVEMENTS) && defined(PLATFORM_DPLAY)
            // Added: the dedicated host went on to a level we don't have
            if ( jkMain_aMissingMap[0] )
            {
                static char16_t wMsg[256];
                char aMsg[256];
                stdString_snprintf(aMsg, sizeof(aMsg), "The server went on to %s, which you don't have.", jkMain_aMissingMap);
                stdString_CharToWchar(wMsg, aMsg, 255);
                wMsg[255] = 0;
                v6 = wMsg;
                jkMain_aMissingMap[0] = 0;
            }
#endif
            jkGuiDialog_ErrorDialog(v4, v6);
            return;
        }

        // Added: Reset crouch toggle when entering a newly loaded level.
        sithControl_ResetCrouchToggle();

        // MOTS added:
        //sithWorld_GetMemoryUsage(sithWorld_g_pCurrentWorld,local_44,local_88);

        if ( !sithNet_isMulti )
        {
            jkPlayer_Startup();
            jkPlayer_InitForceBins();
            jkMain_bInit = 1;
            if ( jkSmack_gameMode == 2 || !jkSmack_gameMode )
            {
                sithCamera_ResetAllCameras();
                jkPlayer_InitSaber();
                sithOpenPostProcess();
            }
        }
        else if ( sithNet_isServer )
        {
LABEL_28:
            sithInventory_InitInventory(sithPlayer_g_pLocalPlayerThing);
            jkPlayer_MpcInitBins(sithPlayer_g_pLocalPlayer);
            
            jkPlayer_Startup();
            jkPlayer_InitForceBins();
            jkMain_bInit = 1;
            if ( jkSmack_gameMode == 2 || !jkSmack_gameMode )
            {
                sithCamera_ResetAllCameras();
                jkPlayer_InitSaber();
                sithOpenPostProcess();
            }
            if ( sithNet_isMulti )
            {
                if ( sithNet_isServer )
                {
                    DirectPlay_SetSessionFlagidk(1);
                    v5 = idx_13b4_related;
                    if ( idx_13b4_related >= (unsigned int)jkPlayer_maxPlayers )
                        v5 = jkPlayer_maxPlayers;
                    DirectPlay_SetSessionDesc(jkMain_aLevelJklFname, v5);
                }
                if ( sithNet_isMulti )
                    jkDSS_wrap_SendSaberInfo_alt();
            }
        }
        else {
            jkMain_bInModalGui = 1;
            stdControl_SetActivation(0);
#if !defined(TARGET_NO_MULTIPLAYER_MENUS)
            if ( jkGuiMultiplayer_ShowSynchronizing() == 1 )
            {
                jkMain_bInModalGui = 0;
                stdControl_SetActivation(1);
                goto LABEL_28;
            }
#endif
            sithClose();
            sithMulti_Shutdown();
            if ( jkGuiRend_menuNestDepth )
                jkGuiRend_bStateChangePending = 1;
            jkSmack_stopTick = 1;
            jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
            jkMain_bInModalGui = 0;
            return;
        }

        if (Main_bMotsCompat) {
            sithPlayer_SetInvItemAmount(SITHBIN_NEW_STARS, 0);
            if (jkMain_motsIdk[0] != 0) {
                stdString_SafeStrCopy(jkMain_aLevelJklFnameMots, jkMain_aLevelJklFname,128);
                stdString_SafeStrCopy(jkMain_aLevelJklFname,jkMain_motsIdk,128);
                if (jkGuiRend_menuNestDepth != 0) {
                    jkGuiRend_bStateChangePending = 1;
                }
                jkMain_aLevelJklFname[127] = '\0';
                jkSmack_stopTick = 1;
                jkSmack_nextGuiState = JK_GAMEMODE_MOTS_CUTSCENE;
                return;
            }
        }
    }

    if ( jkMain_SetVideoMode() )
    {
        stdControl_SetActivation(1);
        stdControl_Flush();
        jkGame_Update();
        jkMain_bLevelActive = 1;
    }
    else
    {
        if ( jkGuiRend_menuNestDepth )
            jkGuiRend_bStateChangePending = 1;
        jkSmack_stopTick = 1;
        jkMain_bLevelActive = 1;
        jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
    }
}

void jkMain_GameplayTick(int a2)
{
    unsigned int v1; // esi
    int v3; // esi

    if (jkMain_bInModalGui) {
        return;
    }

    if (!jkMain_bLevelActive) {
        return;
    }

#ifdef DC_AUTOEXIT_MS
    // Added: quit to menu after N ms of gameplay (headless soak/teardown tests;
    // pairs with DC_AUTOBOOT_MAP, e.g. -DDC_AUTOEXIT_MS=90000).
    {
        static uint32_t dcAutoExitStartMs = 0;
        uint32_t nowMs = stdPlatform_GetTimeMsec();
        if (!dcAutoExitStartMs)
            dcAutoExitStartMs = nowMs;
        if (nowMs - dcAutoExitStartMs > (DC_AUTOEXIT_MS)) {
            dcAutoExitStartMs = nowMs;
#ifdef DC_AUTOEXIT_NEXTMAP
            stdPlatform_Printf("DC_AUTOEXIT: transitioning to " DC_AUTOEXIT_NEXTMAP "\n");
            jkMain_LoadLevelSingleplayer("JK1", DC_AUTOEXIT_NEXTMAP);
#else
            stdPlatform_Printf("DC_AUTOEXIT: leaving gameplay\n");
            jkMain_MenuReturn();
#endif
            return;
        }
    }
#endif

    v1 = stdPlatform_GetTimeMsec();

    if (v1 > jkMain_lastTickMs + TICKRATE_MS)
    {
        jkMain_lastTickMs = v1;
        if (sithUpdate()) return;
    }
    
    if ( g_sithMode == 5 )
    {
        if ( jkGuiRend_menuNestDepth )
            jkGuiRend_bStateChangePending = 1;
        jkSmack_stopTick = 1;
        jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
    }
    else
    {
        if ( sithMulti_bTimelimitMet )
        {
            sithMulti_bTimelimitMet = 0;
            if ( sithNet_isServer )
                jkDSS_SendEndLevel();
        }
        if ( sithMain_bEndLevel )
        {
            sithMain_bEndLevel = 0;
            if (Main_bMotsCompat)
                jkPlayer_idkEndLevel(); // MOTS added
            jkMain_EndLevel(1);
        }
        jkPlayer_nullsub_1(&playerThings[playerThingIdx]);
        jkGame_dword_552B5C += stdPlatform_GetTimeMsec() - v1;
        v3 = stdPlatform_GetTimeMsec();
        if ( g_app_suspended && a2 != 6 ) {
#ifdef SDL2_RENDER
            if (jkMain_lastTickMs == v1)
#endif
            jkGame_Update();
        }
        jkGame_updateMsecsTotal += stdPlatform_GetTimeMsec() - v3;
    }
}

// MOTS altered
void jkMain_GameplayLeave(int a2, int a3)
{
    int v3; // eax

    // MOTS added
    if (a3 == JK_GAMEMODE_MOTS_CUTSCENE) return;

    if ( a3 == JK_GAMEMODE_ESCAPE )
    {
        stdControl_SetActivation(0);
        sithSoundMixer_StopAll();
    }
    if ( jkGame_isDDraw )
    {
        Windows_ShutdownGdi();
        Video_SwitchToGDI();
        jkPlayer_Close();
        jkGame_isDDraw = 0;
    }
    if ( a3 != 6 && jkMain_bInit )
    {
        jkPlayer_Shutdown();
        sithClose();
        jkMain_bInit = 0;
        jkMain_bLevelActive = 0;
    }
    if ( sithNet_isMulti && a3 != 6 )
    {
        jkMain_bLevelActive = 0;
        if ( a3 == 3 ) {
            // MOTS added
            if (Main_bMotsCompat) {
                sithMulti_LobbyMessage();
            }
            sithMulti_Shutdown();
        }
        else {
            sithMulti_LobbyMessage();
        }
        if ( sithNet_isServer )
            DirectPlay_SetSessionFlagidk(0);
        jkMain_bInModalGui = 1;
#ifdef PLATFORM_DPLAY
        if ( stdComm_DPlay_RejoinAsked() ) // Added: as above
            v3 = -1;
        else
#endif
        v3 = jkGuiMultiTally_Show(sithNet_isMulti);
        jkMain_bInModalGui = 0;
        if ( v3 == -1 )
        {
            sithMulti_Shutdown();
            if ( jkGuiRend_menuNestDepth )
                jkGuiRend_bStateChangePending = 1;
            jkSmack_stopTick = 1;
            jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
        }
    }
}

void jkMain_TitleShow(int a1, int a2)
{
    jkGuiTitle_ShowLoadingStatic();
    sithOpenStatic("static.jkl");
    jkHudInv_InitItems(); // MOTS inlined?
}

void jkMain_TitleTick(int a1)
{
    jkGuiTitle_LoadingFinalize();
    if ( jkGuiRend_menuNestDepth )
        jkGuiRend_bStateChangePending = 1;
    jkSmack_stopTick = 1;
    jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
}

void jkMain_TitleLeave(int a1, int a2)
{
    ;
}

void jkMain_MainShow(int a1, int a2)
{
    stdControl_ShowMouseCursor(1);
    stdControl_SetActivation(0); // Added
    jkGuiMain_Show();
}

void jkMain_MainTick(int a1)
{
    ;
}

void jkMain_MainLeave(int a1, int a2)
{
    ;
}

void jkMain_ChoiceShow(int a1, int a2)
{
    int v1; // [esp+0h] [ebp-4h] BYREF

    if ( jkGuiForce_Show(0, 0.0, 1, 0, &v1, 1) == -1 )
    {
        if ( jkGuiRend_menuNestDepth )
            jkGuiRend_bStateChangePending = 1;
        jkSmack_stopTick = 1;
        jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
    }
    else
    {
        jkMain_StartNextLevelInEpisode(0, v1);
    }
}

void jkMain_ChoiceTick(int a1)
{
    ;
}

void jkMain_ChoiceLeave(int a1, int a2)
{
    ;
}

void jkMain_UnkShow(int a1, int a2)
{
    jkPlayer_SetAmmoMaximums(0); // MOTS added
}

void jkMain_UnkTick(int a1)
{
    jkRes_LoadGob(jkMain_strIdk);
    if ( jkEpisode_mLoad.paEntries )
    {
        JK_FREE(jkEpisode_mLoad.paEntries);
        jkEpisode_mLoad.paEntries = 0;

        // Added: prevent UAF
        jkMain_pEpisodeEnt = NULL;
        jkMain_pEpisodeEnt2 = NULL;
    }
    jkEpisode_Load(&jkEpisode_mLoad);

    jkSmack_gameMode = 1;
    if ( jkGuiRend_menuNestDepth )
        jkGuiRend_bStateChangePending = 1;
    jkSmack_stopTick = 1;
    jkSmack_nextGuiState = JK_GAMEMODE_GAMEPLAY;
}

void jkMain_UnkLeave(int a1, int a2)
{
    ;
}

int jkMain_sub_403470(char *a1)
{
    int result; // eax

    sithInventory_g_bInitInventory = 1;
    _strncpy(jkMain_aLevelJklFname, a1, 0x7Fu);
    result = 0;
    jkMain_aLevelJklFname[127] = 0;
    jkSmack_gameMode = 0;
    if ( jkGuiRend_menuNestDepth )
        jkGuiRend_bStateChangePending = 1;
    jkSmack_stopTick = 1;
    jkSmack_nextGuiState = JK_GAMEMODE_GAMEPLAY;
    return result;
}

int jkMain_LoadFile(char *a1)
{
    if (jkRes_LoadCD(1))
    {
        sithInventory_g_bInitInventory = 1;
        jkRes_LoadGob(a1);
        if ( jkEpisode_mLoad.paEntries )
        {
            JK_FREE(jkEpisode_mLoad.paEntries);
            jkEpisode_mLoad.paEntries = 0;

            // Added: prevent UAF
            jkMain_pEpisodeEnt = NULL;
            jkMain_pEpisodeEnt2 = NULL;
        }
        if ( jkEpisode_Load(&jkEpisode_mLoad) )
        {
            return jkMain_StartNextLevelInEpisode(1, 1);
        }
        else
        {
            // Added: %s wants char16_t*, a1 is narrow
            char16_t wTmp[128];
            stdString_CharToWchar(wTmp, a1, 127);
            wTmp[127] = 0;
            Windows_ErrorMsgboxWide("ERR_CANNOT_LOAD_FILE %s", wTmp);
            return 0;
        }
    }
    return 0;
}

int jkMain_loadFile2(char *pGobPath, char *pEpisodeName)
{
    BOOL v2; // esi
    int result; // eax

    _strncpy(jkMain_aLevelJklFname, pEpisodeName, 0x7Fu);
    jkMain_aLevelJklFname[127] = 0;
    jkSmack_gameMode = 2;
    jkRes_LoadGob(pGobPath);
    if ( jkEpisode_mLoad.paEntries )
    {
        JK_FREE(jkEpisode_mLoad.paEntries);
        jkEpisode_mLoad.paEntries = 0;

        // Added: prevent UAF
        jkMain_pEpisodeEnt = NULL;
        jkMain_pEpisodeEnt2 = NULL;
    }
    v2 = jkEpisode_Load(&jkEpisode_mLoad);
    jkEpisode_idk4(&jkEpisode_mLoad, pEpisodeName);
    if ( v2 )
    {
        result = 1;
        jkPlayer_bLoadingSomething = 1;
        if ( jkGuiRend_menuNestDepth )
            jkGuiRend_bStateChangePending = 1;
        jkSmack_stopTick = 1;
        jkSmack_nextGuiState = 5;
    }
    else
    {
        // Added: %s wants char16_t*, pGobPath is narrow
        char16_t wTmp[128];
        stdString_CharToWchar(wTmp, pGobPath, 127);
        wTmp[127] = 0;
        Windows_ErrorMsgboxWide("ERR_CANNOT_LOAD_FILE %s", wTmp);
        result = 0;
    }
    return result;
}

// Added
int jkMain_LoadLevelSingleplayer(char *pGobPath, char *pEpisodeName)
{
    BOOL v2; // esi
    int result; // eax

    _strncpy(jkMain_aLevelJklFname, pEpisodeName, 0x7Fu);
    jkMain_aLevelJklFname[127] = 0;
    jkSmack_gameMode = 0;
    jkRes_LoadGob(pGobPath);
    if ( jkEpisode_mLoad.paEntries )
    {
        JK_FREE(jkEpisode_mLoad.paEntries);
        jkEpisode_mLoad.paEntries = 0;

        // Added: prevent UAF
        jkMain_pEpisodeEnt = NULL;
        jkMain_pEpisodeEnt2 = NULL;
    }
    v2 = jkEpisode_Load(&jkEpisode_mLoad);
    jkEpisode_idk4(&jkEpisode_mLoad, pEpisodeName);
    if ( v2 )
    {
        result = 1;
        jkPlayer_bLoadingSomething = 1;
        if ( jkGuiRend_menuNestDepth )
            jkGuiRend_bStateChangePending = 1;
        jkSmack_stopTick = 1;
        jkSmack_nextGuiState = 5;
    }
    else
    {
        // Added: %s wants char16_t*, pGobPath is narrow
        char16_t wTmp[128];
        stdString_CharToWchar(wTmp, pGobPath, 127);
        wTmp[127] = 0;
        Windows_ErrorMsgboxWide("ERR_CANNOT_LOAD_FILE %s", wTmp);
        result = 0;
    }
    return result;
}

#if defined(QOL_IMPROVEMENTS) && defined(PLATFORM_DPLAY)
// Added: a dedicated server's map rotation ("maps" in server.json, Main_NextRotationMap)
// and admin map changes (Main/jkServer.c), across episodes too. A map is written
// "GOB:level.jkl" here; "level" or "GOB:level" as typed (jkMain_FindLevel).

// In the episode that's loaded.
static int jkMain_LevelExists(const char* pJkl)
{
    char path[160];
    intptr_t f;
    if ( !jkRes_episodeGobName[0] )
        return 0;
    stdString_snprintf(path, sizeof(path), "jkl\\%s", pJkl);
    f = pHS->fileOpen(path, "r");
    if ( !f )
        return 0;
    pHS->fileClose(f);
    return 1;
}

// In episode\<pGobFile> (with its extension), without loading it as the episode.
static int jkMain_GobHasLevel(const char* pGobFile, const char* pJkl)
{
    char path[160], entry[160];
    Gob* pGob;
    int bFound = 0;
    stdString_snprintf(path, sizeof(path), "episode%c%s", LEC_PATH_SEPARATOR_CHR, pGobFile);
    stdString_snprintf(entry, sizeof(entry), "jkl\\%s", pJkl);
    if ( !(pGob = stdGob_Load(path, 2, 0)) )
        return 0;
#ifndef STDGOB_COMPACT_ENTRIES
    for ( uint32_t i = 0; i < pGob->numFiles && !bFound; i++ )
        bFound = !__strcmpi(pGob->entries[i].fname, entry);
#endif
    stdGob_Free(pGob);
    return bFound;
}

// Finds "level" or "GOB:level" (".jkl" and ".gob" optional): in GOB, else in
// pPrefer (an episode name, may be NULL), the loaded episode, then every GOB in
// the episode folder. pOut gets "GOB:level.jkl" with the GOB's name as it's
// spelled on disk. 0 if it's nowhere.
int jkMain_FindLevel(const char* pSpec, const char* pPrefer, char* pOut, size_t outLen)
{
    char aGob[64] = {0}, aJkl[64], aTry[3][64];
    const char* pColon = strchr(pSpec, ':');
    stdFileSearch* pSearch;
    stdFileSearchResult res;
    char* pExt;
    int numTry = 0;

    while ( *pSpec == ' ' ) pSpec++;
    if ( pColon )
    {
        stdString_SafeStrCopy(aGob, pSpec, (size_t)(pColon - pSpec) + 1 < sizeof(aGob) ? (size_t)(pColon - pSpec) + 1 : sizeof(aGob));
        if ( (pExt = strrchr(aGob, '.')) && (!__strcmpi(pExt, ".gob") || !__strcmpi(pExt, ".goo")) ) *pExt = 0;
        pSpec = pColon + 1;
    }
    stdString_SafeStrCopy(aJkl, pSpec, sizeof(aJkl) - 4);
    if ( !aJkl[0] )
        return 0;
    if ( !strchr(aJkl, '.') )
        strcat(aJkl, ".jkl");

    if ( aGob[0] )
        stdString_SafeStrCopy(aTry[numTry++], aGob, 64);
    else
    {
        if ( pPrefer && pPrefer[0] ) stdString_SafeStrCopy(aTry[numTry++], pPrefer, 64);
        if ( jkRes_episodeGobName[0] ) stdString_SafeStrCopy(aTry[numTry++], jkRes_episodeGobName, 64);
    }
    for ( int i = 0; i < numTry; i++ )
    {
        if ( __strcmpi(aTry[i], jkRes_episodeGobName) || !jkMain_LevelExists(aJkl) )
            continue;
        stdString_snprintf(pOut, outLen, "%s:%s", jkRes_episodeGobName, aJkl); // also a folder episode
        return 1;
    }
    if ( !(pSearch = stdFileUtil_NewFind("episode", 3, JKRES_GOB_EXT)) )
        return 0;
    // The ones to try first, then (without a GOB given) all of them.
    for ( int pass = 0; pass < 2; pass++ )
    {
        if ( pass == 1 )
        {
            if ( aGob[0] )
                break;
            stdFileUtil_DisposeFind(pSearch);
            if ( !(pSearch = stdFileUtil_NewFind("episode", 3, JKRES_GOB_EXT)) )
                return 0;
        }
        while ( stdFileUtil_FindNext(pSearch, &res) )
        {
            char aName[64];
            int bTry = (pass == 1);
            if ( res.is_subdirectory || res.fpath[0] == '.' )
                continue;
            stdString_SafeStrCopy(aName, res.fpath, sizeof(aName));
            if ( (pExt = strrchr(aName, '.')) ) *pExt = 0;
            for ( int i = 0; i < numTry && !bTry; i++ )
                bTry = !__strcmpi(aName, aTry[i]);
            if ( !bTry || !jkMain_GobHasLevel(res.fpath, aJkl) )
                continue;
            stdFileUtil_DisposeFind(pSearch);
            stdString_snprintf(pOut, outLen, "%s:%s", aName, aJkl);
            return 1;
        }
    }
    stdFileUtil_DisposeFind(pSearch);
    return 0;
}

// "GOB:level.jkl" -> its parts.
static void jkMain_SplitMap(const char* pMap, char* pGob, size_t gobLen, char* pJkl, size_t jklLen)
{
    const char* pColon = strchr(pMap, ':');
    if ( !pColon )
    {
        stdString_SafeStrCopy(pGob, jkRes_episodeGobName, gobLen);
        stdString_SafeStrCopy(pJkl, pMap, jklLen);
        return;
    }
    stdString_SafeStrCopy(pGob, pMap, (size_t)(pColon - pMap) + 1 < gobLen ? (size_t)(pColon - pMap) + 1 : gobLen);
    stdString_SafeStrCopy(pJkl, pColon + 1, jklLen);
}

// Loads another episode (its GOB and episode.jk) for the level pJkl, as joining a
// game does (jkMain_loadFile2). 0 if it can't be loaded.
static int jkMain_LoadEpisode(const char* pGob, const char* pJkl)
{
    char aGob[64];
    stdString_SafeStrCopy(aGob, pGob, sizeof(aGob));
    jkRes_LoadGob(aGob);
    if ( jkEpisode_mLoad.paEntries )
    {
        JK_FREE(jkEpisode_mLoad.paEntries);
        jkEpisode_mLoad.paEntries = 0;
        jkMain_pEpisodeEnt = NULL;
        jkMain_pEpisodeEnt2 = NULL;
    }
    if ( !jkEpisode_Load(&jkEpisode_mLoad) )
        return 0;
    jkEpisode_idk4(&jkEpisode_mLoad, (char*)pJkl);
    jkPlayer_bLoadingSomething = 1;
    return 1;
}

static int jkMain_SwitchEpisode(const char* pGob, const char* pJkl)
{
    char aOld[64];
    if ( !__strcmpi(pGob, jkRes_episodeGobName) )
        return 1;
    stdString_SafeStrCopy(aOld, jkRes_episodeGobName, sizeof(aOld));
    stdPlatform_Printf("OpenJKDF2: episode %s -> %s\n", aOld, pGob);
    if ( jkMain_LoadEpisode(pGob, pJkl) )
        return 1;
    jkMain_LoadEpisode(aOld, jkMain_aLevelJklFname); // back to the one we had
    return 0;
}

static char jkMain_aNextMap[128];    // host: the next map ("GOB:level.jkl"), picked when the level ended
static char jkMain_aForcedMap[128];  // host: the next map an admin picked (console "map"/"nextmap")

static int jkMain_IsDedicatedHost(void);

// Rotation entries (Main_NextRotationMap): a bare level is looked for in the
// server's first episode before the loaded one.
static char jkMain_aHomeEpisode[64];
static int jkMain_ResolveRotationMap(const char* pSpec, char* pOut, size_t outLen)
{
    if ( !jkMain_aHomeEpisode[0] )
        stdString_SafeStrCopy(jkMain_aHomeEpisode, jkRes_episodeGobName, sizeof(jkMain_aHomeEpisode));
    return jkMain_FindLevel(pSpec, jkMain_aHomeEpisode, pOut, outLen);
}

// Console "map"/"nextmap" (Main/jkServer.c): the next level, after this one ends.
// 0 if there's no such level.
int jkMain_SetNextMap(const char* pSpec)
{
    return jkMain_FindLevel(pSpec, NULL, jkMain_aForcedMap, sizeof(jkMain_aForcedMap));
}

// What the next level will be ("GOB:level.jkl"), as far as it's decided: an admin's pick, else NULL.
const char* jkMain_GetForcedNextMap(void)
{
    return jkMain_aForcedMap[0] ? jkMain_aForcedMap : NULL;
}

// The host ends the level (jkDSS_SendEndLevel; it goes on for 10 s, then the
// scores screen). A dedicated host picks the next map now (an admin's pick, else
// its rotation's) and names it in the game's description, so its joiners have it
// by the time they load it. Without either, everyone goes on as the episode says.
void jkMain_LevelEnding(void)
{
    char aGob[64], aJkl[64];
    const char* pNext;
    jkMain_aNextMap[0] = 0;
    if ( !sithNet_isMulti || !sithNet_isServer || !jkGuiNetHost_bIsDedicated )
        return;
    if ( jkMain_aForcedMap[0] )
    {
        stdString_SafeStrCopy(jkMain_aNextMap, jkMain_aForcedMap, sizeof(jkMain_aNextMap));
        jkMain_aForcedMap[0] = 0;
    }
    else if ( (pNext = Main_NextRotationMap(jkMain_ResolveRotationMap)) )
        stdString_SafeStrCopy(jkMain_aNextMap, pNext, sizeof(jkMain_aNextMap));
    else
    {
        stdComm_DPlay_SetSessionMap(NULL, NULL); // joiners go on as the episode says
        return;
    }
    jkMain_SplitMap(jkMain_aNextMap, aGob, sizeof(aGob), aJkl, sizeof(aJkl));
    stdComm_DPlay_SetSessionMap(aGob, aJkl);
    stdPlatform_Printf("OpenJKDF2: next map %s\n", jkMain_aNextMap);
}

// The host goes on to the next map it named (jkMain_LevelEnding), from another
// episode too. 0 if it hasn't named one.
static int jkMain_HostGoesOn(void)
{
    char aGob[64], aJkl[64];
    if ( !jkMain_aNextMap[0] )
        return 0;
    jkMain_SplitMap(jkMain_aNextMap, aGob, sizeof(aGob), aJkl, sizeof(aJkl));
    jkMain_aNextMap[0] = 0;
    if ( !jkMain_SwitchEpisode(aGob, aJkl) )
    {
        // Found when it was picked, gone now: the server's first episode and map again
        stdPlatform_Printf("OpenJKDF2: can't load episode %s\n", aGob);
        return 0;
    }
    stdString_SafeStrCopy(jkMain_aLevelJklFname, aJkl, sizeof(jkMain_aLevelJklFname));
    return 1;
}

// A joiner of a dedicated host, about to load the next level: the one the host
// named when it ended the level, if it did (only through the Imperial Network
// Protocol, where description changes reach us), in its episode. Otherwise its own
// pick stands: the same level, or the episode's next, as the host's.
static void jkMain_FollowHostMap(void)
{
    char aGob[64], aJkl[128];
    jkMain_aMissingMap[0] = 0;
    if ( sithNet_isServer || !jkMain_IsDedicatedHost() || !stdComm_DPlay_GetSessionNextMap(aGob, sizeof(aGob), aJkl, sizeof(aJkl)) )
        return;
    if ( !__strcmpi(aJkl, jkMain_aLevelJklFname) && (!aGob[0] || !__strcmpi(aGob, jkRes_episodeGobName)) )
        return;
    stdPlatform_Printf("OpenJKDF2: the host goes on with %s:%s\n", aGob, aJkl);
    // Added: an episode we lack, or have another copy of: the host's, if it serves it
    if ( aGob[0] && __strcmpi(aGob, jkRes_episodeGobName) )
        stdComm_DPlay_FetchHostMap(aGob);
    if ( aGob[0] && !jkMain_SwitchEpisode(aGob, aJkl) )
    {
        stdString_snprintf(jkMain_aMissingMap, sizeof(jkMain_aMissingMap), "%s (%s)", aJkl, aGob);
        stdPlatform_Printf("OpenJKDF2: no episode %s here\n", aGob);
    }
    else if ( !jkMain_LevelExists(aJkl) )
        stdString_snprintf(jkMain_aMissingMap, sizeof(jkMain_aMissingMap), "%s (%s)", aJkl, aGob);
    stdString_SafeStrCopy(jkMain_aLevelJklFname, aJkl, sizeof(jkMain_aLevelJklFname));
}

// Joiners: does the host go on to a level it named? (Then its episode's own
// order, which may end there, doesn't matter.)
static int jkMain_HostNamedNextMap(void)
{
    char aGob[64], aJkl[128];
    return sithNet_isMulti && !sithNet_isServer && jkMain_IsDedicatedHost()
        && stdComm_DPlay_GetSessionNextMap(aGob, sizeof(aGob), aJkl, sizeof(aJkl));
}
#endif

#ifdef QOL_IMPROVEMENTS
// Added: the game's host is a dedicated server. Joiners see it as the host's
// slot never being in the game (sithMulti_StartupServer sets it to 6, sent as 4).
static int jkMain_IsDedicatedHost(void)
{
    if ( sithNet_isServer )
        return jkGuiNetHost_bIsDedicated;
    for (int i = 0; i < jkPlayer_maxPlayers; i++)
    {
        if ( jkPlayer_playerInfos[i].playerNetId && jkPlayer_playerInfos[i].playerNetId == sithNet_serverNetId )
            return !(jkPlayer_playerInfos[i].flags & 1);
    }
    return 0;
}
#endif

int jkMain_StartNextLevelInEpisode(int a1, int bIsAPath)
{
    jkEpisodeEntry *v2; // eax
    jkEpisodeEntry *v3; // ecx
    int v4; // eax
    signed int result; // eax

    if ( !jkEpisode_mLoad.numSeq )
    {
        if ( jkGuiRend_menuNestDepth )
        {
            jkGuiRend_bStateChangePending = 1;
        }
        jkSmack_stopTick = 1;
        jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
        return 0;
    }
    if ( a1 )
    {
        v2 = jkEpisode_GetCurrentEpisodeEntry(&jkEpisode_mLoad);
        v3 = v2;
        jkMain_pEpisodeEnt = v2;
        jkMain_pEpisodeEnt2 = v2;
        jkPlayer_bLoadingSomething = 0;
    }
    else
    {
        v3 = jkMain_pEpisodeEnt;
        v2 = jkMain_pEpisodeEnt2;
    }
    if ( jkPlayer_bLoadingSomething )
    {
        jkMain_pEpisodeEnt = jkEpisode_GetCurrentEpisodeEntry(&jkEpisode_mLoad);
        v2 = jkEpisode_GetNextEntryInDecisionPath(&jkEpisode_mLoad, bIsAPath);
        v3 = jkMain_pEpisodeEnt;
        jkMain_pEpisodeEnt2 = v2;
        jkPlayer_bLoadingSomething = 0;
    }
#if defined(QOL_IMPROVEMENTS) && defined(PLATFORM_DPLAY)
    // Added: a dedicated server's next map (an admin's pick or its rotation's, from
    // another episode too): the host goes on with the one it picked when the level
    // ended, and its joiners follow (jkMain_FollowHostMap), whatever the episode's
    // own order says (a one-level episode would end the game here).
    if ( sithNet_isMulti && ((sithNet_isServer && jkMain_HostGoesOn()) || jkMain_HostNamedNextMap()) )
    {
        jkSmack_gameMode = 2;
        if ( jkGuiRend_menuNestDepth )
            jkGuiRend_bStateChangePending = 1;
        jkSmack_stopTick = 1;
        jkSmack_nextGuiState = JK_GAMEMODE_GAMEPLAY;
        return 1;
    }
#endif
    if ( !v2 )
    {
        v4 = jkGuiRend_menuNestDepth;
        if ( v3->gotoA == -1 )
        {
            if ( jkGuiRend_menuNestDepth )
                jkGuiRend_bStateChangePending = 1;
            jkSmack_stopTick = 1;
            jkSmack_nextGuiState = JK_GAMEMODE_CREDITS;
            return 1;
        }
        if ( v4 )
            jkGuiRend_bStateChangePending = 1;

        jkSmack_stopTick = 1;
        jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
        return 0;
    }
    if ( sithNet_isMulti && (sithNet_MultiModeFlags & MULTIMODEFLAG_SINGLE_LEVEL) != 0 )
    {
#ifdef QOL_IMPROVEMENTS
        // Added: with a dedicated host the level starts over instead of the game
        // ending (which left the server idle with nothing to join). Everyone in the
        // game works out the next level by itself, so joiners do the same.
        if ( jkMain_IsDedicatedHost() )
        {
            jkSmack_gameMode = 2;
            if ( jkGuiRend_menuNestDepth )
                jkGuiRend_bStateChangePending = 1;
            jkSmack_stopTick = 1;
            jkSmack_nextGuiState = JK_GAMEMODE_GAMEPLAY; // jkMain_aLevelJklFname is still this level
            return 1;
        }
#endif
        v4 = jkGuiRend_menuNestDepth;
        
        if ( v4 )
            jkGuiRend_bStateChangePending = 1;
        jkSmack_stopTick = 1;
        jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
        return 0;
    }
    if ( v3->level == v2->level || jkSmack_currentGuiState == JK_GAMEMODE_ENDLEVEL )
    {
        if ( v2->type == 1 && jkSmack_currentGuiState == JK_GAMEMODE_GAMEPLAY )
        {
            if ( jkGuiRend_menuNestDepth )
                jkGuiRend_bStateChangePending = 1;
            jkSmack_stopTick = 1;
            jkSmack_nextGuiState = JK_GAMEMODE_CD_SWITCH;
            result = 1;
        }
        else
        {
            jkPlayer_bLoadingSomething = 1;
            jkMain_cd_swap_reverify(v2);
            result = 1;
        }
    }
    else
    {
        if ( jkGuiRend_menuNestDepth )
            jkGuiRend_bStateChangePending = 1;
        jkSmack_stopTick = 1;
        jkSmack_nextGuiState = JK_GAMEMODE_ENDLEVEL;
        result = 1;
    }
    return result;
}

int jkMain_cd_swap_reverify(jkEpisodeEntry *ent)
{
    int v1; // eax
    int v2; // eax
    signed int result; // eax
    signed int v4; // edi
    int v5; // edi
    signed int v6; // esi
    char16_t *v7; // eax
    char16_t *v8; // [esp-4h] [ebp-94h]
    char v9[128]; // [esp+10h] [ebp-80h] BYREF

    v1 = ent->type;
    if ( !v1 )
    {
        v5 = 0;
        v6 = 0;
        while ( !v6 )
        {
            if ( Windows_installType < 9 )
                v6 = jkRes_LoadCD(ent->cdNum);
            else
                v6 = 1;
            if ( !v6 )
            {
                v8 = jkStrings_GetUniStringWithFallback("GUI_CONFIRM_ABORTCD");
                v7 = jkStrings_GetUniStringWithFallback("GUI_ABORTCDREQUEST");
                if ( jkGuiDialog_YesNoDialog(v7, v8) )
                    v5 = 1;
            }
            if ( v5 )
            {
                if ( !v6 )
                {
                    if ( jkGuiRend_menuNestDepth )
                        jkGuiRend_bStateChangePending = 1;
                    jkSmack_stopTick = 1;
                    jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
                    return 1;
                }
                break;
            }
        }
        stdString_SafeStrCopy(jkMain_aLevelJklFname, ent->fileName, 128);
        jkSmack_gameMode = sithNet_isMulti != 0 ? 2 : 0;
        if ( jkGuiRend_menuNestDepth )
            jkGuiRend_bStateChangePending = 1;
        jkSmack_stopTick = 1;
        jkSmack_nextGuiState = JK_GAMEMODE_GAMEPLAY;
        return 1;
    }
    v2 = v1 - 1;
    if ( v2 )
    {
        if ( v2 == 1 )
        {
            if ( jkGuiRend_menuNestDepth )
                jkGuiRend_bStateChangePending = 1;
            jkSmack_stopTick = 1;
            jkSmack_nextGuiState = JK_GAMEMODE_CHOICE; // force select/choice?
            return 1;
        }
        return 1;
    }

    // Added: Move down
    //if ( jkPlayer_setDisableCutscenes )
    //    v4 = 0;
    //else
    //    v4 = jkRes_LoadCD(ent->cdNum);

    jkPlayer_WriteConfSwap(&playerThings[playerThingIdx], ent->cdNum, ent->fileName);
    // Added: Move down
    //if ( !v4 )
    //    return jkMain_StartNextLevelInEpisode(0, 1);

    // Added: Cutscenes disabled
    if ( jkPlayer_setDisableCutscenes )
        return jkMain_StartNextLevelInEpisode(0, 1);

    _sprintf(v9, "video%c%s", 92, ent->fileName);
    if ( !util_FileExists(v9) ) {
        // Added: check file first before asking for CDs
        v4 = jkRes_LoadCD(ent->cdNum);

        if ( !v4 ) {
            return jkMain_StartNextLevelInEpisode(0, 1);
        }

        if ( !util_FileExists(v9) ) {
            return jkMain_StartNextLevelInEpisode(0, 1);
        }
    }
    jkRes_FileExists(v9, jkMain_aLevelJklFname, 128);
    switch ( jkSmack_currentGuiState )
    {
        case 3:
        case 9:
            if ( jkGuiRend_menuNestDepth )
                jkGuiRend_bStateChangePending = 1;
            jkSmack_stopTick = 1;
            jkSmack_nextGuiState = JK_GAMEMODE_VIDEO4;
            result = 1;
            break;
        case 5:
        case 7:
        case 8:
            if ( jkGuiRend_menuNestDepth )
                jkGuiRend_bStateChangePending = 1;
            jkSmack_stopTick = 1;
            jkSmack_nextGuiState = JK_GAMEMODE_VIDEO3;
            result = 1;
            break;
        default:
            return 1;
    }
    return result;
}

int jkMain_SetMap(int levelNum)
{
    jkEpisode_EndLevel(&jkEpisode_mLoad, levelNum);
    return jkMain_cd_swap_reverify(jkEpisode_GetCurrentEpisodeEntry(&jkEpisode_mLoad));
}

void jkMain_do_guistate6()
{
    if ( !jkSmack_stopTick )
    {
        if ( jkGuiRend_menuNestDepth )
            jkGuiRend_bStateChangePending = 1;
        jkSmack_stopTick = 1;
        jkSmack_nextGuiState = JK_GAMEMODE_ESCAPE;
    }
}

int jkMain_sub_4034D0(char *a1, char *a2, char *a3, char16_t *a4)
{
    sithInventory_g_bInitInventory = 0;
    _strncpy(jkMain_aLevelJklFname, a2, 0x7Fu);
    jkMain_aLevelJklFname[127] = 0;
    _strncpy(jkMain_strIdk, a1, 0x7Fu);
    jkMain_strIdk[127] = 0;
    _strncpy(gamemode_1_str, a3, 0x7Fu);
    gamemode_1_str[127] = 0;
    _wcsncpy(jkMain_wstrIdk, a4, 0x7Fu);

    jkMain_wstrIdk[127] = 0;
    jkPlayer_bLoadingSomething = 1;
    if ( jkGuiRend_menuNestDepth )
        jkGuiRend_bStateChangePending = 1;
    jkSmack_stopTick = 1;
    jkSmack_nextGuiState = JK_GAMEMODE_UNK;
    return 1;
}

int jkMain_MissionReload()
{
    signed int result; // eax

    result = 1;
    if ( jkGuiRend_menuNestDepth )
        jkGuiRend_bStateChangePending = 1;
    jkSmack_stopTick = 1;
    jkSmack_nextGuiState = JK_GAMEMODE_GAMEPLAY;
    return result;
}

int jkMain_MenuReturn()
{
    signed int result; // eax

    result = 1;
    if ( jkGuiRend_menuNestDepth )
        jkGuiRend_bStateChangePending = 1;
    jkSmack_stopTick = 1;
    jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
    return result;
}

int jkMain_EndLevel(int bIsAPath)
{
    if (!Main_bMotsCompat && jkEpisode_mLoad.numSeq )
    {
        jkEpisodeEntry* pEpisodeEnt = jkEpisode_GetCurrentEpisodeEntry(&jkEpisode_mLoad);
        if ( pEpisodeEnt->darkpow || pEpisodeEnt->lightpow )
        {
            if ( pEpisodeEnt->lightpow )
            {
                if ( pEpisodeEnt->lightpow >= SITHBIN_FP_START && pEpisodeEnt->lightpow <= SITHBIN_FP_END && jkPlayer_GetChoice() != 2 )
                    sithInventory_SetCarries(playerThings[playerThingIdx].actorThing, pEpisodeEnt->lightpow, 1);
            }
            if ( pEpisodeEnt->darkpow )
            {
                if ( pEpisodeEnt->darkpow >= SITHBIN_FP_START && pEpisodeEnt->darkpow <= SITHBIN_FP_END && jkPlayer_GetChoice() != 1 )
                    sithInventory_SetCarries(playerThings[playerThingIdx].actorThing, pEpisodeEnt->darkpow, 1);
            }
        }
    }

    if (Main_bMotsCompat) {
        jkPlayer_idkEndLevel();
    }

    return jkMain_StartNextLevelInEpisode(0, bIsAPath);
}

void jkMain_CdSwitchShow(int a1, int a2)
{
    jkMain_StartNextLevelInEpisode(0, 1);
}

// MOTS altered
void jkMain_VideoShow(int a1, int a2)
{
    signed int result; // eax

    // Added: Fix a bug with the door on Level 10?
    //if (Main_bMotsCompat && !sithNet_isMulti )
    //    sithTime_Pause();

    result = jkCutscene_sub_421310(jkMain_aLevelJklFname);
    if ( !result )
    {
        // Added: %s wants char16_t*, jkMain_aLevelJklFname is narrow
        char16_t wTmp[128];
        stdString_CharToWchar(wTmp, jkMain_aLevelJklFname, 127);
        wTmp[127] = 0;
        Windows_ErrorMsgboxWide("ERR_CANNOT_LOAD_FILE %s", wTmp);
        switch ( a1 )
        {
            case JK_GAMEMODE_VIDEO:
                result = 1;
                if ( jkGuiRend_menuNestDepth )
                    jkGuiRend_bStateChangePending = 1;
                jkSmack_stopTick = 1;
                jkSmack_nextGuiState = JK_GAMEMODE_TITLE;
                break;
            case JK_GAMEMODE_VIDEO2:
                result = 1;
                if ( jkGuiRend_menuNestDepth )
                    jkGuiRend_bStateChangePending = 1;
                jkSmack_stopTick = 1;
                jkSmack_nextGuiState = JK_GAMEMODE_CUTSCENE;
                break;
            case JK_GAMEMODE_VIDEO3:
            case JK_GAMEMODE_VIDEO4:
                result = jkMain_StartNextLevelInEpisode(0, 1);
                break;
            case JK_GAMEMODE_MOTS_CUTSCENE: // MOTS added
                if (jkGuiRend_menuNestDepth != 0) {
                    jkGuiRend_bStateChangePending = 1;
                }
                jkSmack_stopTick = 1;
                jkSmack_nextGuiState = JK_GAMEMODE_GAMEPLAY;
                return;
            default:
                result = 1;
                if ( jkGuiRend_menuNestDepth )
                    jkGuiRend_bStateChangePending = 1;
                jkSmack_stopTick = 1;
                jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
                break;
        }
    }
    return;
}

void jkMain_VideoTick(int a2)
{
    signed int result; // eax

    result = jkCutscene_smack_related_loops();
    if ( result )
    {
        result = a2 - 1;
        switch ( a2 )
        {
            case JK_GAMEMODE_VIDEO:
                result = 1;
                if ( jkGuiRend_menuNestDepth )
                    jkGuiRend_bStateChangePending = 1;
                jkSmack_stopTick = 1;
                jkSmack_nextGuiState = JK_GAMEMODE_TITLE;
                break;
            case JK_GAMEMODE_VIDEO2:
                result = 1;
                if ( jkGuiRend_menuNestDepth )
                    jkGuiRend_bStateChangePending = 1;
                jkSmack_stopTick = 1;
                jkSmack_nextGuiState = JK_GAMEMODE_CUTSCENE;
                break;
            case JK_GAMEMODE_VIDEO3:
                result = 1;
                if ( jkGuiRend_menuNestDepth )
                    jkGuiRend_bStateChangePending = 1;
                jkSmack_stopTick = 1;
                jkSmack_nextGuiState = JK_GAMEMODE_ENDLEVEL;
                break;
            case JK_GAMEMODE_VIDEO4:
            case JK_GAMEMODE_MOTS_CUTSCENE: // MOTS added
                result = 1;
                if ( jkGuiRend_menuNestDepth )
                    jkGuiRend_bStateChangePending = 1;
                jkSmack_stopTick = 1;
                jkSmack_nextGuiState = JK_GAMEMODE_GAMEPLAY;
                break;
            default:
                return;
        }
    }
    return;
}

void jkMain_VideoLeave(int a1, int a2)
{
    // Added: Fix a bug with the door on Level 10?
    //if (Main_bMotsCompat && !sithNet_isMulti )
    //    sithTime_Resume();

    jkCutscene_sub_421410();
    if ( a1 == JK_GAMEMODE_VIDEO3 || a1 == JK_GAMEMODE_VIDEO4 )
        jkMain_StartNextLevelInEpisode(0, 1);
}

void jkMain_CreditsShow(int a1, int a2)
{
    if ( !jkCredits_Show() )
    {
        if ( jkGuiRend_menuNestDepth )
            jkGuiRend_bStateChangePending = 1;
        jkSmack_stopTick = 1;
        jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
    }
}

void jkMain_CreditsTick(int a1)
{
    if ( jkCredits_Tick() )
    {
        if ( jkGuiRend_menuNestDepth )
            jkGuiRend_bStateChangePending = 1;
        jkSmack_stopTick = 1;
        jkSmack_nextGuiState = JK_GAMEMODE_MAIN;
    }
}

void jkMain_CreditsLeave(int a1, int a2)
{
    jkCredits_Skip();
}

void jkMain_CutsceneShow(int a1, int a2)
{
    jkGuiMain_ShowCutscenes();
}

void jkMain_CutsceneTick(int a1)
{
    ;
}

void jkMain_CutsceneLeave(int a1, int a2)
{
    ;
}

int jkMain_SwitchTo13()
{
    signed int result; // eax

    result = 1;
    if ( jkGuiRend_menuNestDepth )
        jkGuiRend_bStateChangePending = 1;
    jkSmack_stopTick = 1;
    jkSmack_nextGuiState = JK_GAMEMODE_CREDITS;
    return result;
}

int jkMain_SwitchTo12()
{
    signed int result; // eax

    result = 1;
    if ( jkGuiRend_menuNestDepth )
        jkGuiRend_bStateChangePending = 1;
    jkSmack_stopTick = 1;
    jkSmack_nextGuiState = JK_GAMEMODE_CUTSCENE;
    return result;
}

int jkMain_SwitchTo4(const char *pFpath)
{
    int result; // eax

    jkRes_FileExists(pFpath, jkMain_aLevelJklFname, 128);
    result = 1;
    if ( jkGuiRend_menuNestDepth )
        jkGuiRend_bStateChangePending = 1;
    jkSmack_stopTick = 1;
    jkSmack_nextGuiState = JK_GAMEMODE_VIDEO2;
    return result;
}

// MOTS added
void jkMain_StartupCutscene(char *pCutsceneStr)
{
    char local_80 [128];

    jkPlayer_WriteConfSwap(playerThings + playerThingIdx, 1, pCutsceneStr);
    _sprintf(local_80,"video%c%s", '\\', pCutsceneStr);

    if (util_FileExists(local_80)) {
        jkRes_FileExists(local_80, jkMain_motsIdk, 0x80);
    }
}

#if defined(SDL2_RENDER) || defined(TARGET_RETRO_HOMEBREW)
void jkMain_FixRes()
{
    if (!jkGame_isDDraw)
        return;
    
    uint32_t newW = Window_xSize;
    uint32_t newH = Window_ySize;

    //if (jkGame_isDDraw)
    {
        newW = (uint32_t)((flex_t)Window_xSize * ((480.0*2.0)/Window_ySize));
        newH = 480*2;
    }

    if (newW > Window_xSize)
    {
        newW = Window_xSize;
        newH = Window_ySize;
    }

    if (newW < 640)
        newW = 640;
    if (newH < 480)
        newH = 480;

#ifdef TARGET_DREAMCAST
    newW = 640;
    newH = 480;
#endif

    Video_modeStruct.viewSizeIdx = 0;
    Video_modeStruct.aViewSizes[Video_modeStruct.viewSizeIdx].xMin = 0;
    Video_modeStruct.aViewSizes[Video_modeStruct.viewSizeIdx].yMin = 0;
    Video_modeStruct.aViewSizes[Video_modeStruct.viewSizeIdx].xMax = newW / 2;
    Video_modeStruct.aViewSizes[Video_modeStruct.viewSizeIdx].yMax = newH / 2;
    
    stdDisplay_pCurVideoMode->format.width = newW;
    stdDisplay_pCurVideoMode->format.height = newH;
    stdDisplay_pCurVideoMode->widthMaybe = newW;
    stdDisplay_pCurVideoMode->format.rowWidth = newW;
    stdDisplay_pCurVideoMode->format.rowSize = newW;
    
    Video_menuBuffer.format.rowWidth = newW;
    Video_otherBuf.format.rowWidth = newW;
    Video_menuBuffer.format.rowSize = newW;
    Video_otherBuf.format.rowSize = newW;
    Video_menuBuffer.format.width = newW;
    Video_otherBuf.format.width = newW;
    Video_menuBuffer.format.height = newH;
    Video_otherBuf.format.height = newH;
    
    _memcpy(&Video_format, &stdDisplay_pCurVideoMode->format, sizeof(tRasterInfo));
    _memcpy(&Video_format2, &stdDisplay_pCurVideoMode->format, sizeof(tRasterInfo));
    
    Video_format.width = newW;
    Video_format.height = newH;
    
    jkDev_Close();
    jkHud_Close();
    if (Main_bMotsCompat) {
        jkHudScope_Close();
        jkHudCameraView_Close();
    }
    jkHudInv_Close();
    sithCamera_Close();
    rdCanvas_Free(Video_pCanvas);

#if defined(SDL2_RENDER)
    rdCanvas_Free(Video_pCanvasOverlayMap);
#endif

    jkHudInv_LoadItemRes();
    jkHud_Open();
    if (Main_bMotsCompat) {
        jkHudScope_Open();
        jkHudCameraView_Open();
    }
    jkDev_Open();
    
    Video_pCanvas = rdCanvas_New(2, Video_pMenuBuffer, Video_pVbufIdk, 0, 0, newW, newH, 6);
#if defined(SDL2_RENDER)
    Video_pCanvasOverlayMap = rdCanvas_New(2, Video_pOverlayMapBuffer, Video_pOverlayMapBuffer, 0, 0, newW, newH, 6);
#endif
    sithCamera_Open(Video_pCanvas, stdDisplay_pCurVideoMode->widthMaybe);
}

int jkMain_SetVideoMode()
{
    signed int result; // eax
    char16_t *v1; // eax
    char16_t *v2; // eax
    char16_t *v3; // [esp-4h] [ebp-10h]
    char16_t *v4; // [esp-4h] [ebp-10h]

    if ( jkGame_isDDraw )
        return 0;
    
    /*if ( !sithNet_isMulti )
    {
        jkMain_bInModalGui = 1;
        //sithControl_Close();
        v3 = jkStrings_GetUniStringWithFallback("ERR_CHANGING_VIDEO_DESC");
        v1 = jkStrings_GetUniStringWithFallback("ERR_CHANGING_VIDEO_MODE");
        jkGuiDialog_ErrorDialog(v1, v3);
        //sithControl_Open();
        jkMain_bInModalGui = 0;
    }*/
    
    sithControl_Open();
    sithRender_SetExtraThingRenderFunc(jkPlayer_renderSaberWeaponMesh);

    uint32_t newW = Window_xSize;
    uint32_t newH = Window_ySize;

    //if (jkGame_isDDraw)
    {
        newW = (uint32_t)((flex_t)Window_xSize * ((480.0*2.0)/Window_ySize));
        newH = 480*2;
    }

    if (newW > Window_xSize)
    {
        newW = Window_xSize;
        newH = Window_ySize;
    }

    if (newW < 640)
        newW = 640;
    if (newH < 480)
        newH = 480;

    Video_modeStruct.viewSizeIdx = 0;
    Video_modeStruct.aViewSizes[Video_modeStruct.viewSizeIdx].xMin = 0;
    Video_modeStruct.aViewSizes[Video_modeStruct.viewSizeIdx].yMax = 0;
    Video_modeStruct.aViewSizes[Video_modeStruct.viewSizeIdx].xMax = newW / 2;
    Video_modeStruct.aViewSizes[Video_modeStruct.viewSizeIdx].yMax = newH / 2;

    stdDisplay_pCurVideoMode->format.width = newW;
    stdDisplay_pCurVideoMode->format.height = newH;
    stdDisplay_pCurVideoMode->widthMaybe = newW;
    stdDisplay_pCurVideoMode->format.rowWidth = newW;
    stdDisplay_pCurVideoMode->format.rowSize = newW;
    
    Video_menuBuffer.format.rowWidth = newW;
    Video_otherBuf.format.rowWidth = newW;
    Video_menuBuffer.format.rowSize = newW;
    Video_otherBuf.format.rowSize = newW;
    Video_menuBuffer.format.width = newW;
    Video_otherBuf.format.width = newW;
    Video_menuBuffer.format.height = newH;
    Video_otherBuf.format.height = newH;
    
    _memcpy(&Video_format, &stdDisplay_pCurVideoMode->format, sizeof(tRasterInfo));
    _memcpy(&Video_format2, &stdDisplay_pCurVideoMode->format, sizeof(tRasterInfo));
    
    Video_format.width = newW;
    Video_format.height = newH;
    
    Window_AddMsgHandler(Windows_GdiHandler);
    
    stdPalEffects_RefreshPalette();
    sithRender_SetPalette(stdDisplay_GetPalette());

    jkHudInv_LoadItemRes();
    // Added close
    jkHud_Close();
    if (Main_bMotsCompat) {
        jkHudScope_Close();
        jkHudCameraView_Close();
    }
    jkHud_Open();
    if (Main_bMotsCompat) {
        jkHudScope_Open();
        jkHudCameraView_Open();
    }
    jkDev_Open();
    
    rdroid_curAcceleration = 1;
    Video_pCanvas = rdCanvas_New(2, Video_pMenuBuffer, Video_pVbufIdk, 0, 0, newW, newH, 6);
#if defined(SDL2_RENDER)
    Video_pCanvasOverlayMap = rdCanvas_New(2, Video_pOverlayMapBuffer, Video_pOverlayMapBuffer, 0, 0, newW, newH, 6);
#endif
#ifdef JKM_LIGHTING
    if (Main_bMotsCompat) {
        sithRender_SetRenderFlags(0xaa);
    }
    else {
        sithRender_SetRenderFlags(0x2a);
    }
#else
    sithRender_SetRenderFlags(0x2a);
#endif
    sithRender_SetGeoMode(Video_modeStruct.geoMode);
    sithRender_SetLightingMode(Video_modeStruct.lightMode);
    sithRender_SetTexMode(Video_modeStruct.texMode);
    sithCamera_Open(Video_pCanvas, stdDisplay_pCurVideoMode->widthMaybe);

    stdDisplay_SetMode(0, 0, 0);

    Video_bOpened = 1;
    jkGame_isDDraw = 1;
    return 1;
}
#endif
