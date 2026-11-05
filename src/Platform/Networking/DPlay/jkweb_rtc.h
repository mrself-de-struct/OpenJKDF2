#ifndef _JKWEB_RTC_H
#define _JKWEB_RTC_H

// WebRTC data channels for jkweb_client.c (implementation: jkweb_rtc.c).
// All functions are called from the game thread. Without JKWEB_WEBRTC they
// are stubs and the lobby carries all traffic.

#include <stdint.h>

int  jkrtc_Available(void);
void jkrtc_SetIceServersJson(const char* json);      // from the lobby's "ice" line
void jkrtc_Begin(int isHost, int log);               // joiner: starts the offer to the host
void jkrtc_End(void);
void jkrtc_OnSignal(int from, const char* json);     // decoded "signal" payload
void jkrtc_ClosePeer(int id);
int  jkrtc_IsOpen(int id);
int  jkrtc_Send(int id, const void* data, int size); // 1 if it went over the direct link
int  jkrtc_PollSignal(int* toId, char* out, int cap);       // outgoing signaling, -1 when empty
int  jkrtc_PollPacket(int* fromId, uint8_t* out, int cap);  // incoming packets, -1 when empty
// The "jk-map" channel (map and mod downloads, jkweb_xfer.c).
int  jkrtc_MapOpen(int id);
int  jkrtc_SendMap(int id, const void* data, int size);     // 1 if it went over the direct link
int  jkrtc_MapBuffered(int id);                           // bytes waiting to go out, -1 if not open
int  jkrtc_PollMapFrame(int* fromId, uint8_t* out, int cap); // -1 when empty, -2 dropped (too big)

#endif
