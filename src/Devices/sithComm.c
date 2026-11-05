#include "sithComm.h"

#include "General/stdConffile.h"
#include "Gameplay/sithPlayer.h"
#include "Dss/sithMulti.h"
#include "Dss/sithDSSThing.h"
#include "Dss/sithDSS.h"
#include "Dss/sithDSSCog.h"
#include "Win95/stdComm.h"
#include "Main/Main.h" // Added: Main_bVerboseNetworking
#include "Gameplay/sithTime.h" // Added: sithTime_g_msecGameTime
#ifdef PLATFORM_DPLAY
#include "Platform/Networking/Hits/stdComm_Hits.h" // Added: Hit what you see
#endif
#include "stdPlatform.h"

const char* sithDSS_IdToStr(int id); // sithDSS.c
#include "jk.h"

// Added: the message from another game being handled (its DSS id), 0 between
// messages, so Hit what you see can tell a hit that another game's message
// decided (stdComm_WSRelayHits.c).
int sithComm_msgInProgress = 0;

int sithComm_009a1160 = 0;
int sithComm_version = 6;

// MOTS altered
int sithComm_Startup()
{
    if (sithMessage_bSturtup)
        return 0;
    _memset(sithMessage_aTypeFuncs, 0, sizeof(cogMsg_Handler) * 65);  // TODO define
    _memset(sithComm_aMsgPairs, 0, sizeof(sithCogMsg_Pair) * 0x80); // TODO define
    sithComm_dword_847E84 = 0;
    sithComm_msgId = 1;
    sithMessage_aTypeFuncs[DSS_THINGPOS] = sithDSSThing_ProcessPos;
    sithMessage_aTypeFuncs[DSS_FIREPROJECTILE] = sithDSSThing_ProcessFire;
    sithMessage_aTypeFuncs[DSS_JOINREQUEST] = sithMulti_ProcessJoinRequest;
    sithMessage_aTypeFuncs[DSS_WELCOME] = sithMulti_ProcessWelcome;
    sithMessage_aTypeFuncs[DSS_DEATH] = sithDSSThing_ProcessDeath;
    sithMessage_aTypeFuncs[DSS_DAMAGE] = sithDSSThing_ProcessDamage;
    sithMessage_aTypeFuncs[DSS_SENDTRIGGER] = sithDSSCog_ProcessMessage;
    sithMessage_aTypeFuncs[DSS_SYNCTHING] = sithDSSThing_ProcessStateUpdate;
    sithMessage_aTypeFuncs[DSS_PLAYSOUND] = sithDSSThing_ProcessPlaySound;
    sithMessage_aTypeFuncs[DSS_PLAYKEY] = sithDSSThing_ProcessPlayKey;
    sithMessage_aTypeFuncs[DSS_THINGFULLDESC] = sithDSSThing_ProcessFullDescription;
    sithMessage_aTypeFuncs[DSS_SYNCCOG] = sithDSSCog_ProcessCogState;
    sithMessage_aTypeFuncs[DSS_SURFACESTATUS] = sithDSS_ProcessSurfaceStatus;
    sithMessage_aTypeFuncs[DSS_AISTATUS] = sithDSS_ProcessAIStatus;
    sithMessage_aTypeFuncs[DSS_INVENTORY] = sithDSS_ProcessInventory;
    sithMessage_aTypeFuncs[DSS_SURFACE] = sithDSS_ProcessAnimStatus;
    sithMessage_aTypeFuncs[DSS_SECTORSTATUS] = sithDSS_ProcessSectorStatus;
    sithMessage_aTypeFuncs[DSS_PATHMOVE] = sithDSSThing_ProcessPathMove;
    sithMessage_aTypeFuncs[DSS_SYNCPUPPET] = sithDSS_ProcessPuppetStatus;
    sithMessage_aTypeFuncs[DSS_LEAVEJOIN] = sithMulti_ProcessSyncPlayers;
    sithMessage_aTypeFuncs[DSS_SYNCTHINGATTACHMENT] = sithDSSThing_ProcessAttachment;
    sithMessage_aTypeFuncs[DSS_SYNCEVENTS] = sithDSS_ProcessSyncTaskEvents;
    sithMessage_aTypeFuncs[DSS_SYNCCAMERAS] = sithDSS_ProcessSyncCameras;
    sithMessage_aTypeFuncs[DSS_TAKEITEM1] = sithDSSThing_ProcessTake;
    sithMessage_aTypeFuncs[DSS_TAKEITEM2] = sithDSSThing_ProcessTake;
    sithMessage_aTypeFuncs[DSS_STOPKEY] = sithDSSThing_ProcessStopKey;
    sithMessage_aTypeFuncs[DSS_STOPSOUND] = sithDSSThing_ProcessStopSound;
    sithMessage_aTypeFuncs[DSS_CREATETHING] = sithDSSThing_ProcessCreateThing;
    sithMessage_aTypeFuncs[DSS_SYNCPALEFFECTS] = sithDSS_ProcessSyncPalEffects;
    sithMessage_aTypeFuncs[DSS_ID_1F] = sithDSS_ProcessSyncGameState;
    sithMessage_aTypeFuncs[DSS_CHAT] = sithMulti_ProcessChat;
    sithMessage_aTypeFuncs[DSS_DESTROYTHING] = sithDSSThing_ProcessDestroyThing;
    sithMessage_aTypeFuncs[DSS_SECTORFLAGS] = sithDSS_ProcessSectorFlags;
    sithMessage_aTypeFuncs[DSS_PLAYSOUNDMODE] = sithDSSThing_ProcessPlaySoundMode;
    sithMessage_aTypeFuncs[DSS_PLAYKEYMODE] = sithDSSThing_ProcessPlayKeyMode;
    sithMessage_aTypeFuncs[DSS_SETTHINGMODEL] = sithDSSThing_ProcessSetModel;
    sithMessage_aTypeFuncs[DSS_PING] = sithMulti_ProcessPing;
    sithMessage_aTypeFuncs[DSS_PINGREPLY] = sithMulti_ProcessPong;
    sithMessage_aTypeFuncs[DSS_ENUMPLAYERS] = stdComm_cogMsg_HandleEnumPlayers;
    sithMessage_aTypeFuncs[DSS_RESET] = sithComm_cogMsg_Reset;
    sithMessage_aTypeFuncs[DSS_QUIT] = sithMulti_ProcessQuit;

    if (Main_bMotsCompat) {
        sithMessage_aTypeFuncs[DSS_MOTS_NEW_1] = sithDSSThing_ProcessMOTSNew1;
        sithMessage_aTypeFuncs[DSS_MOTS_NEW_2] = sithDSSThing_ProcessMOTSNew2;
    }

    // Added: clean reset
    sithComm_009a1160 = 0;
    sithComm_version = 6;

    sithMessage_bSturtup = 1;
    return 1;
}

void sithMessage_Shutdown()
{
    if ( sithMessage_bSturtup )
        sithMessage_bSturtup = 0;

    // Added: clean reset
    sithComm_009a1160 = 0;
    sithComm_version = 6;
}

#ifdef SITHCOMM_HEAP_MSGBUF
// Added: shadow the generated 66KB .bss retry buffer with a lazily-allocated
// heap buffer (multiplayer-only; see engine_config.h). The generated array
// becomes unreferenced and --gc-sections strips it.
static SithMessage* sithComm_pMsgTmpBufHeap = NULL;
#define sithComm_MsgTmpBuf sithComm_pMsgTmpBufHeap
static int sithComm_EnsureMsgTmpBuf(void)
{
    if (!sithComm_pMsgTmpBufHeap)
    {
        // On DC an OOM here purges the material cache and retries internally.
        sithComm_pMsgTmpBufHeap = (SithMessage*)SITH_ALLOC(32 * sizeof(SithMessage));
        if (sithComm_pMsgTmpBufHeap)
            _memset(sithComm_pMsgTmpBufHeap, 0, 32 * sizeof(SithMessage));
    }
    return sithComm_pMsgTmpBufHeap != NULL;
}
#endif

void sithMessage_RegisterFunction(int type, cogMsg_Handler pFunc)
{
    sithMessage_aTypeFuncs[type] = pFunc;
}

// MOTS altered
int sithComm_SendMsgToPlayer(SithMessage *pMessage, int idTo, int outstream, int dwDPFlags)
{
    char multiplayerFlags; // bl
    unsigned int curMs; // esi
    __int16 v9; // ax
    int idx; // ecx
    SithMessage *v14; // eax
    SithMessage *v17; // edi
    int v19; // ecx
    int v20; // eax
    int idx_; // [esp+18h] [ebp+Ch]

    //printf("sithComm_SendMsgToPlayer %x %x %x %x\n", msg->netMsg.cogMsgId, a2, mpFlags, a4);

    int ret = 1;
    multiplayerFlags = sithMessage_g_outputstream & outstream;
    if (!multiplayerFlags)
        return 1;
    curMs = sithTime_g_msecGameTime;
    pMessage->netMsg.idx = playerThingIdx;
    pMessage->netMsg.timeMs = curMs;
    if ( (multiplayerFlags & 1) != 0 )
    {
        if ( dwDPFlags )
        {
            v9 = sithComm_msgId;
            if ( !sithComm_msgId )
                v9 = 1;
            pMessage->netMsg.msgId = v9;
            sithComm_msgId = v9 + 1;
            pMessage->netMsg.field_C = idTo;
            idx_ = 0;
            pMessage->netMsg.timeMs2 = curMs;
            pMessage->netMsg.field_14 = 0;
            for (int i = 0; i < jkPlayer_maxPlayers; i++)
            {
                if ( i != playerThingIdx && (jkPlayer_playerInfos[i].playerNetId == idTo || (idTo == -1 || !idTo) && (jkPlayer_playerInfos[i].flags & 1) != 0) )
                    pMessage->netMsg.field_14 |= 1 << i;
                // Added: Dedicated server hax. Altered (from tristamus/OpenJKDF2 4574909d): only for
                // messages to everyone. A message sent to one other player also got resent to the
                // host, which then ran it as if meant for it (a Carbonite hit between two joiners
                // froze the host too).
#ifdef PLATFORM_DPLAY
                // Altered: and on any connection, a joiner waits for the host's ack of a message to
                // everyone even when the host (dedicated) keeps its slot out of the game
                if ((stdComm_bOpenJkdf2Wire && !i && i != playerThingIdx && (idTo == -1 || !idTo))
                    || (!sithNet_isServer && i != playerThingIdx && (idTo == -1 || !idTo)
                        && jkPlayer_playerInfos[i].playerNetId && jkPlayer_playerInfos[i].playerNetId == sithNet_serverNetId))
#else
                if (!i && i != playerThingIdx && (idTo == -1 || !idTo))
#endif
                {
                    pMessage->netMsg.field_14 |= 1 << i;
                }
            }
            if ( !pMessage->netMsg.field_14 )
                goto LABEL_35;
#ifdef SITHCOMM_HEAP_MSGBUF
            // Added: first reliable send allocates the retry buffer; if that
            // somehow fails, degrade to an untracked (unreliable) send.
            if ( !sithComm_EnsureMsgTmpBuf() )
                goto LABEL_35;
#endif
            for (idx = 0; idx < 32; idx++)
            {
                v14 = &sithComm_MsgTmpBuf[idx];
                if ( !v14->netMsg.msgId )
                    break;
                if ( v14->netMsg.timeMs < curMs )
                {
                    curMs = v14->netMsg.timeMs;
                    idx_ = idx;
                }
                ++v14;
            }

            if ( idx == 32 )
            {
                v17 = &sithComm_MsgTmpBuf[idx_];
                v17->netMsg.field_18++;
                v17->netMsg.timeMs2 = sithTime_g_msecGameTime;
                for (unsigned int v15 = 0; v15 < jkPlayer_maxPlayers; v15++)
                {
                    v19 = sithComm_MsgTmpBuf[idx_].netMsg.field_14;
                    if ( (v19 & (1 << v15)) != 0 )
                    {
                        if (jkPlayer_playerInfos[v15].playerNetId)
                            stdComm_SendToPlayer(v17, jkPlayer_playerInfos[v15].playerNetId);
                        else
                            sithComm_MsgTmpBuf[idx_].netMsg.field_14 = ~(1 << v15) & v19;
                    }
                }
                if ( !sithComm_MsgTmpBuf[idx_].netMsg.field_14 || sithComm_MsgTmpBuf[idx_].netMsg.field_18 >= 6u )
                {
                    _memset(v17, 0, sizeof(SithMessage));
                    --sithComm_idk2;
                }
                idx = idx_;
                --sithComm_idk2;
            }
            ++sithComm_idk2;
            v20 = pMessage->netMsg.field_14;
            _memcpy(&sithComm_MsgTmpBuf[idx_], pMessage, sizeof(SithMessage));
            if ( !v20 )
LABEL_35:
                pMessage->netMsg.msgId = 0;
        }
        else
        {
            pMessage->netMsg.msgId = 0;
        }
        ret = stdComm_SendToPlayer(pMessage, idTo);
    }
    if ( (multiplayerFlags & 4) != 0 )
    {
        sithMessage_FileWrite(pMessage);
    }
    return ret;
}

// MOTS altered
void sithMessage_FileWrite(SithMessage* pMessage)
{
    // Added: multiple version handling
    if (sithComm_version == 0x7D6) {
        stdConffile_Write((const char*)&sithComm_009a1160, sizeof(sithComm_009a1160));
    }
    stdConffile_Write((const char*)&pMessage->netMsg.cogMsgId, sizeof(int));
    stdConffile_Write((const char*)&pMessage->netMsg.msg_size, sizeof(int));
    stdConffile_Write((const char*)&pMessage->pktData[0], pMessage->netMsg.msg_size);
}

// MOTS altered
int sithMessage_ProcessMessages()
{
    int v1; // eax
    uint16_t v2; // dx
    uint32_t *v3; // ecx
    int v4; // eax
    int v12; // ecx
    int v13; // [esp+4h] [ebp-4h]

    v13 = 0;
    sithMessage_bStopProcessMessages = 0;
    if ( !sithMessage_g_inputstream )
        return 0;
    while ( stdComm_Recv(&sithComm_netMsgTmp) == 1 )
    {
        ++v13;
        if ( sithComm_netMsgTmp.netMsg.idx )
        {
            v1 = sithPlayer_GetPlayerNum(sithComm_netMsgTmp.netMsg.idx);
            v2 = sithComm_netMsgTmp.netMsg.cogMsgId;
            if ( v1 >= 0 )
            {
                jkPlayer_playerInfos[v1].msecLastCommTime = sithTime_g_msecGameTime;
LABEL_14:
                if ( sithComm_netMsgTmp.netMsg.msgId )
                {
                    sithComm_MsgTmpBuf2.netMsg.msgId = 0;
                    *(uint16_t*)sithComm_MsgTmpBuf2.pktData = sithComm_netMsgTmp.netMsg.msgId;
                    sithComm_MsgTmpBuf2.netMsg.field_C = sithComm_netMsgTmp.netMsg.idx;
                    sithComm_MsgTmpBuf2.netMsg.cogMsgId = DSS_RESET;
                    sithComm_MsgTmpBuf2.netMsg.msg_size = 2;
                    stdComm_SendToPlayer(&sithComm_MsgTmpBuf2, sithComm_netMsgTmp.netMsg.idx);
                    
                    int i = 0;
                    v4 = (uint16_t)sithComm_netMsgTmp.netMsg.msgId;
                    while ( sithComm_netMsgTmp.netMsg.idx != sithComm_aMsgPairs[i].idx || (uint16_t)sithComm_netMsgTmp.netMsg.msgId != sithComm_aMsgPairs[i].msgId )
                    {
                        i++;
                        if ( i >= 128 )
                        {
                            sithComm_aMsgPairs[sithComm_dword_847E84].idx = sithComm_netMsgTmp.netMsg.idx;
                            sithComm_aMsgPairs[sithComm_dword_847E84].msgId = v4;
                            sithComm_dword_847E84++;
                            if ( sithComm_dword_847E84 >= 0x80 )
                                sithComm_dword_847E84 = 0;
                            v2 = sithComm_netMsgTmp.netMsg.cogMsgId;
                            goto LABEL_22;
                        }
                    }
                    // Added: say when a guaranteed message is dropped as a repeat
                    if (Main_bVerboseNetworking)
                        stdPlatform_Printf("sithComm: dropped repeat msg %u (%s) from %x\n", (unsigned)v4,
                                           sithDSS_IdToStr(v2), (unsigned)sithComm_netMsgTmp.netMsg.idx);
                }
                else
                {
LABEL_22:
#ifdef PLATFORM_DPLAY
                    // Added: Hit what you see: another player's swing, animation and sound
                    // messages wait until this game draws them at the moment they were sent.
                    if ( stdComm_WSRelay_HoldForTimeline(&sithComm_netMsgTmp) )
                        goto LABEL_25;
#endif
                    if ( v2 < (unsigned int)DSS_MAX )
                    {
                        if ( sithMessage_aTypeFuncs[v2] )
                        {
                            sithComm_msgInProgress = v2; // Added
                            sithMessage_aTypeFuncs[v2](&sithComm_netMsgTmp);
                            sithComm_msgInProgress = 0; // Added
                        }
                    }
#ifdef PLATFORM_DPLAY
                    // Added: Hit what you see's own messages (Platform/Networking/Hits),
                    // which older games ignore.
                    else if ( v2 == STDCOMM_WSRELAY_HITS_MSG )
                    {
                        sithComm_msgInProgress = v2;
                        stdComm_WSRelay_ProcessHits(&sithComm_netMsgTmp);
                        sithComm_msgInProgress = 0;
                    }
#endif
                }
                goto LABEL_25;
            }
            if ( sithComm_netMsgTmp.netMsg.cogMsgId == DSS_WELCOME
              || sithComm_netMsgTmp.netMsg.cogMsgId == DSS_JOINREQUEST
              || sithComm_netMsgTmp.netMsg.cogMsgId == DSS_RESET
              || sithComm_netMsgTmp.netMsg.cogMsgId == DSS_LEAVEJOIN
              || (g_submodeFlags & 8) != 0 )
            {
                goto LABEL_14;
            }
#ifdef QOL_IMPROVEMENTS
            // Altered: in a level's first 30 s, a player still finishing the last one
            // (a slow machine, a browser tab in the background) sends its last
            // messages from there; it rejoins once it has loaded this level. JK
            // ejected it ("Ejected from game") for those.
            if ( sithNet_isServer && sithTime_g_msecGameTime < 30000 )
            {
                static uint32_t lastIgnored = 0;
                if ( lastIgnored != sithComm_netMsgTmp.netMsg.idx )
                    stdPlatform_Printf("OpenJKDF2: ignoring messages from %x, still on the last level\n", (unsigned)sithComm_netMsgTmp.netMsg.idx);
                lastIgnored = sithComm_netMsgTmp.netMsg.idx;
            }
            else
#endif
            if ( sithNet_isServer )
                sithMulti_QuitPlayer(sithComm_netMsgTmp.netMsg.idx);
        }
LABEL_25:
        if ( sithMessage_bStopProcessMessages )
            break;
    }
    sithComm_SyncWithPlayers();
    return v13;
}

// Added (from tristamus/OpenJKDF2 9e24db78): forget the reliable messages seen from a player who left. Every
// game numbers its messages from 1, and the next player given the same net id
// (the lobby hands out the lowest free one) had their first messages taken
// for repeats of the last player's and dropped, so in a quiet game a quick
// rejoin timed out with "Server has left game".
void sithComm_ForgetSender(int idx)
{
    if ( !idx )
        return;

    for (int i = 0; i < 0x80; i++)
    {
        if ( sithComm_aMsgPairs[i].idx == (uint32_t)idx )
        {
            sithComm_aMsgPairs[i].idx = 0;
            sithComm_aMsgPairs[i].msgId = 0;
        }
    }
}

void sithMessage_StopProcessMessages()
{
    sithMessage_bStopProcessMessages = 1;
}

int sithMessage_Process(SithMessage *pMessage)
{
    int result; // eax

    int msgId = pMessage->netMsg.cogMsgId;

    if ( (signed int)(uint16_t)msgId < 65 && sithMessage_aTypeFuncs[msgId])
        result = sithMessage_aTypeFuncs[msgId](pMessage);
    else
        result = 1;
    return result;
}

void sithComm_SyncWithPlayers()
{
#ifdef SITHCOMM_HEAP_MSGBUF
    if ( !sithComm_MsgTmpBuf ) // Added: nothing buffered yet
        return;
#endif
    if ( sithComm_idk2 )
    {
        
        for (int i = 0; i < 32; i++)
        {
            if (!sithComm_MsgTmpBuf[i].netMsg.msgId)
                continue;

            if ( sithComm_MsgTmpBuf[i].netMsg.timeMs2 + 1700 <= sithTime_g_msecGameTime )
            {
                sithComm_MsgTmpBuf[i].netMsg.field_18++;
                sithComm_MsgTmpBuf[i].netMsg.timeMs2 = sithTime_g_msecGameTime;

                for (int v9 = 0; v9 < jkPlayer_maxPlayers; v9++)
                {
                    if (sithComm_MsgTmpBuf[i].netMsg.field_14 & (1 << v9))
                    {
                        if (jkPlayer_playerInfos[v9].playerNetId)
                            stdComm_SendToPlayer(&sithComm_MsgTmpBuf[i], jkPlayer_playerInfos[v9].playerNetId);
                        else
                            sithComm_MsgTmpBuf[i].netMsg.field_14 &= ~(1 << v9);
                    }
                }

                if ( !sithComm_MsgTmpBuf[i].netMsg.field_14 || sithComm_MsgTmpBuf[i].netMsg.field_18 >= 6 )
                {
                    _memset(&sithComm_MsgTmpBuf[i], 0, sizeof(SithMessage));
                    --sithComm_idk2;
                }
            }
        }
    }
}

void sithComm_ClearMsgTmpBuf()
{
#ifdef SITHCOMM_HEAP_MSGBUF
    // Added: called at MP session teardown -- give the 66KB back to the heap.
    if ( sithComm_MsgTmpBuf )
    {
        SITH_FREE(sithComm_MsgTmpBuf);
        sithComm_MsgTmpBuf = NULL;
    }
#else
    _memset(sithComm_MsgTmpBuf, 0, sizeof(sithComm_MsgTmpBuf));
#endif
    sithComm_idk2 = 0;

    // Added: also forget the messages seen in the last game, for the same
    // reason as sithComm_ForgetSender: a host's next game reuses net ids.
    _memset(sithComm_aMsgPairs, 0, sizeof(sithCogMsg_Pair) * 0x80);
    sithComm_dword_847E84 = 0;
}

int sithComm_cogMsg_Reset(SithMessage *msg)
{
    int v1; // edi
    char playerIdx; // al
    
    int foundIdx;

    NETMSG_IN_START(msg);

    v1 = NETMSG_POPS16();
    playerIdx = sithPlayer_GetPlayerNum(msg->netMsg.idx);
    foundIdx = 0;
#ifdef SITHCOMM_HEAP_MSGBUF
    if ( !sithComm_MsgTmpBuf ) // Added: no tracked messages to ack
        return 1;
#endif
    for (foundIdx = 0; foundIdx < 32; foundIdx++)
    {
        if (sithComm_MsgTmpBuf[foundIdx].netMsg.msgId == v1 )
            break;
    }

    if ( foundIdx != 32 )
    {
        sithComm_MsgTmpBuf[foundIdx].netMsg.field_14 &= ~(1 << playerIdx);
        if (!sithComm_MsgTmpBuf[foundIdx].netMsg.field_14)
        {
            _memset(&sithComm_MsgTmpBuf[foundIdx], 0, sizeof(SithMessage));
            --sithComm_idk2;
        }
    }

    return 1;
}