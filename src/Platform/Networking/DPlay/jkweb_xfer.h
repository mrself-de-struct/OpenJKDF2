#ifndef _JKWEB_XFER_H
#define _JKWEB_XFER_H

// Map and mod transfers on the web lobby, the jkdf2mp browser build's way
// (packaging/wasm/maps.js and mods.js in github.com/tristamus/OpenJKDF2):
// a joiner downloads the host's (one download at a time), a host serves its
// own. jkweb_xfer.c.

#include <stddef.h>
#include <stdint.h>

#define JKX_KIND_MAP   0
#define JKX_KIND_MOD   1

#define JKX_VIA_DIRECT 1   // the "jk-map" WebRTC channel
#define JKX_VIA_RELAY  2   // the lobby

#define JKX_BUSY       0
#define JKX_READY      1
#define JKX_FAILED     2
#define JKX_ASK        3   // jkx_SetAskFirst: waiting for jkx_Answer()

// Big enough for any frame: a 64 KiB block part (or 48 KiB lobby chunk) plus headers.
#define JKX_MAX_FRAME  (64 * 1024 + 1024)   // a 64 KiB block part and its headers

// Gets the host's episode GOB "<name>.<ext>" into episode/ unless ours is
// already the same file (or it's an original episode we have).
void jkx_StartMap(const char* name, const char* ext);
// Gets the host's mod (pack fingerprint fp) into netmods/<first 16 hex digits>.
void jkx_StartMod(const uint8_t fp[32], const char* name, const char* ext);
// 0 an original episode, 1 a custom map we have a copy of, 2 one that downloads on joining.
int  jkx_MapState(const char* name, const char* ext);
// Moves the download along; call often. Returns JKX_BUSY/READY/FAILED.
int  jkx_Tick(void);
void jkx_Cancel(void);
// Our own: with bAsk set, a download stops (JKX_ASK) once the host has said what it is,
// until jkx_Answer(1) goes ahead or jkx_Answer(0) gives up ("declined", no message).
// jkx_AskText() is the question, with the name and size.
void jkx_SetAskFirst(int bAsk);
const char* jkx_AskText(void);
void jkx_Answer(int bYes);
// One line for the progress dialog.
const char* jkx_Status(void);
// After JKX_FAILED: a reason code (as maps.js/mods.js name them) and the text
// to show the player ("" when there's nothing to say, e.g. cancelled).
const char* jkx_Reason(void);
const char* jkx_Message(void);
// After a mod reached JKX_READY: the folder it was unpacked into.
const char* jkx_ResultPath(void);

// Host side: what joiners can get from us for this game, worked out once
// (the episode GOB, and the running mod: a folder, or one file when
// modIsFile; modPath NULL for none). Requests are answered from then on
// until jkx_HostWithdraw().
void jkx_HostOffer(const char* mapName, const char* ext, const char* modPath, int modIsFile,
                   const uint8_t* modFp, const char* modName);
void jkx_HostWithdraw(void);
// Sends some of the chunks joiners asked for, as the link takes them without
// waiting (call on every network poll; jkweb_client.c does).
void jkx_HostPump(void);

// A "JKMT" frame from player `from` (jkweb_client.c): requests go to the
// host side, answers to the download in progress.
void jkx_OnFrame(uint32_t from, const uint8_t* p, size_t n, int via);

// Provided by the transport (stdComm_dplay.c picks the web lobby's, jkweb_client.c's
// jkweb_LobbyXfer*, or the Imperial Network Protocol's; or the tests):
// sends a frame to player `to`, over the direct link if allowed and open;
// returns JKX_VIA_* or 0 if it couldn't be sent.
int  jkweb_XferSend(uint32_t to, const uint8_t* frame, size_t n, int allowDirect);
int  jkweb_XferCanSend(uint32_t to, int allowDirect); // a frame to `to` would go out now without waiting
void jkweb_XferNoteLoss(uint32_t to); // Added: `to` asked again for a block it was sent (some was lost)
uint32_t jkweb_XferPartMax(void);     // Added: largest block part to send at once, a multiple of 16 KiB (0: as asked)
int  jkweb_XferDirectUp(void);       // the "jk-map" channel to the host is open
int  jkweb_XferDirectPossible(void); // a direct link could still come up
int  jkweb_XferHostState(void);      // 1 ok, 0 host left, -1 lobby connection lost
void jkweb_XferPump(void);           // reads what's waiting (calls jkx_OnFrame)

// The web lobby's (jkweb_client.c).
int  jkweb_LobbyXferSend(uint32_t to, const uint8_t* frame, size_t n, int allowDirect);
int  jkweb_LobbyXferCanSend(uint32_t to, int allowDirect);
int  jkweb_LobbyXferDirectUp(void);
int  jkweb_LobbyXferDirectPossible(void);
int  jkweb_LobbyXferHostState(void);
void jkweb_LobbyXferPump(void);

#endif
