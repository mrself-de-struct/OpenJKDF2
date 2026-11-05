// Offline tests for the native DirectPlay client: feeds DirectPlay messages
// captured from real JK.EXE sessions through the parser, and checks that the
// messages we build are byte-identical to what JK.EXE sent.
//
// Build (Linux):
//   cc -std=gnu11 -DPLATFORM_DPLAY -DDPLAY_NATIVE -DDPLAY_NATIVE_TESTS
//      -Isrc/Platform/Networking/DPlay src/Tests/dplay_native_test.c
//      src/Platform/Networking/DPlay/dplay_native.c -o dplay_native_test
//   ./dplay_native_test <dir with captured *.bin messages>
// Each .bin file is 4 bytes of source IPv4 address followed by one message;
// the ipx_*.bin ones (JK.EXE with IPXWrapper) then have the source UDP port
// (2 bytes, big-endian) and a whole IPXWrapper datagram.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include "dplay_shim.h"

void dpn_TestFeed(const uint8_t* msg, uint32_t size, uint32_t srcIpNet, int asWaitCmd);
void dpn_TestBegin(const uint8_t appGuid[16]);
int dpn_TestPlayers(void (*fn)(uint32_t, uint32_t, uint32_t, const uint16_t*, uint32_t, uint16_t, uint16_t));
const dps_session* dpn_TestDesc(void);
const dps_session* dpn_TestEnumed(int i);
int dpn_TestBuildCreatePlayer(uint8_t* out, uint32_t id, uint32_t sysId, const uint16_t* name);
int dpn_TestBuildAddForwardRequest(uint8_t* out, uint32_t sysId, uint32_t reserved1);
int dpn_TestHostEnumReply(uint8_t* out, const uint8_t guid[16], uint32_t key, const dps_session* d, uint32_t curPlayers);
int dpn_TestHostPlayerReply(uint8_t* out, uint32_t id);
int dpn_TestHostAddForward(uint8_t* out, uint32_t id, uint32_t ipNet);
int dpn_TestBuildAddForwardRequestPw(uint8_t* out, uint32_t sysId, const uint16_t* pw, uint32_t reserved1);
void dpn_TestIpxBegin(const uint8_t appGuid[16], const uint8_t addr[12]);
int dpn_TestIpxPacket(const uint8_t* pkt, uint32_t n, uint32_t ipNet, uint16_t port, int asWaitCmd, uint8_t* sent, uint32_t* sentLen);
uint32_t dpn_TestReplyId(void);
int dpn_TestIpxPeer(uint32_t handle, uint8_t addr[12], uint32_t* ipNet, uint16_t* port);

#define DPN_TEST_DESC_GUID (28 + 8)   // guidInstance inside a captured ENUMSESSIONSREPLY
static const uint8_t kJk[16] = { 0xC0,0x13,0x06,0xBF,0x79,0xDE,0xD0,0x11,0x99,0xC9,0x00,0xA0,0x24,0x76,0xAD,0x4B };
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } else { printf("ok:   " __VA_ARGS__); printf("\n"); } } while (0)

static uint8_t* load(const char* dir, const char* name, uint32_t* size, uint32_t* ip)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "rb");
    if (!f) { printf("missing %s\n", path); exit(2); }
    static uint8_t buf[8][8192];
    static int slot = 0;
    uint8_t* b = buf[slot++ & 7];
    size_t n = fread(b, 1, 8192, f);
    fclose(f);
    memcpy(ip, b, 4);
    *size = (uint32_t)n - 4;
    return b + 4;
}

static void str8(const uint16_t* s, char* out, size_t n)
{
    size_t i = 0;
    for (; s && s[i] && i + 1 < n; i++) out[i] = (char)s[i];
    out[i] = 0;
}

static void printPlayer(uint32_t id, uint32_t flags, uint32_t sys, const uint16_t* name, uint32_t ip, uint16_t tcp, uint16_t udp)
{
    char n[64]; struct in_addr a; a.s_addr = ip;
    str8(name, n, sizeof(n));
    printf("      %08x flags=%x sys=%08x %-12s %s:%u/%u\n", id, flags, sys, n, inet_ntoa(a), tcp, udp);
}

static int findCount; static uint32_t want; static char wantName[32]; static uint32_t wantIp, foundIp;
static void finder(uint32_t id, uint32_t flags, uint32_t sys, const uint16_t* name, uint32_t ip, uint16_t tcp, uint16_t udp)
{
    char n[64]; (void)flags; (void)sys; (void)tcp; (void)udp;
    str8(name, n, sizeof(n));
    if (id == want && !strcmp(n, wantName) && ip == wantIp) findCount++;
}

// Over IPX a player's address is a peer handle; finds the one for "want".
static void ipxFinder(uint32_t id, uint32_t flags, uint32_t sys, const uint16_t* name, uint32_t ip, uint16_t tcp, uint16_t udp)
{
    char n[64]; (void)flags; (void)sys; (void)tcp; (void)udp;
    str8(name, n, sizeof(n));
    if (id == want && !strcmp(n, wantName)) { findCount++; foundIp = ip; }
}

static uint8_t* loadIpx(const char* dir, const char* name, uint32_t* size, uint32_t* ip, uint16_t* port)
{
    uint8_t* b = load(dir, name, size, ip);
    *port = (uint16_t)((b[0] << 8) | b[1]);
    *size -= 2;
    return b + 2;
}

static void hex(const char* h, uint8_t* out) { for (; h[0] && h[1]; h += 2) sscanf(h, "%2hhx", out++); }

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "/tmp/dpt";
    uint32_t size, ip;
    uint8_t* m;
    char s[256];

    dpn_TestBegin(kJk);

    // EnumSessionsReply (host -> client)
    m = load(dir, "c_00_01.bin", &size, &ip);
    dpn_TestFeed(m, size, ip, 0);
    const dps_session* e = dpn_TestEnumed(0);
    CHECK(e != NULL, "enum reply parsed");
    if (e)
    {
        str8(e->name, s, sizeof(s));
        CHECK(!strcmp(s, "deeznuts:JK1MP:m10.jkl"), "session name '%s'", s);
        CHECK(e->flags == 0xC0, "session flags 0x%x", e->flags);
        CHECK(e->maxPlayers == 16 && e->curPlayers == 2, "players %u/%u", e->curPlayers, e->maxPlayers);
        CHECK((e->user[2] & 0xFFFF) == 10, "version field %u", e->user[2] & 0xFFFF);
        printf("      dwUser = %08x %08x %08x %08x\n", e->user[0], e->user[1], e->user[2], e->user[3]);
    }

    // SuperEnumPlayersReply (host -> client), as if we were waiting for it
    m = load(dir, "c_05_29.bin", &size, &ip);
    dpn_TestFeed(m, size, ip, 0x29);
    printf("      players after superenum:\n");
    int np = dpn_TestPlayers(printPlayer);
    CHECK(np == 5, "5 entries in name table (got %d)", np);
    want = 0x38C0B185; strcpy(wantName, "PlayerThree"); inet_pton(AF_INET, "192.0.2.30", &wantIp); findCount = 0;
    dpn_TestPlayers(finder);
    CHECK(findCount == 1, "PlayerThree has id 38c0b185 at 192.0.2.30");
    want = 0x38C0B181; strcpy(wantName, "Annie"); inet_pton(AF_INET, "192.0.2.10", &wantIp); findCount = 0;
    dpn_TestPlayers(finder);
    CHECK(findCount == 1, "Annie (host) has id 38c0b181; her zero address resolves to the host");
    str8(dpn_TestDesc()->name, s, sizeof(s));
    CHECK(!strcmp(s, "deeznuts:JK1MP:m10.jkl"), "session name from superenum '%s'", s);

    // Our CREATEPLAYER must match JK.EXE's byte for byte.
    {
        uint8_t ours[512];
        uint16_t name[] = { 'M','r',' ','C',0 };
        int n = dpn_TestBuildCreatePlayer(ours, 0x38C5B183, 0x38C4B182, name);
        m = load(dir, "c_08_08.bin", &size, &ip);
        CHECK((uint32_t)n == size && !memcmp(ours, m, size), "CREATEPLAYER identical to JK.EXE's (%d vs %u bytes)", n, size);
        if ((uint32_t)n == size)
            for (uint32_t i = 0; i < size; i++)
                if (ours[i] != m[i]) { printf("      first difference at 0x%x: ours %02x theirs %02x\n", i, ours[i], m[i]); break; }
    }

    // Our ADDFORWARDREQUEST must match too (sys id 38c4b182, session dwReserved1 38c1b180).
    {
        uint8_t ours[512];
        int n = dpn_TestBuildAddForwardRequest(ours, 0x38C4B182, 0x38C1B180);
        m = load(dir, "c_04_13.bin", &size, &ip);
        CHECK((uint32_t)n == size && !memcmp(ours, m, size), "ADDFORWARDREQUEST identical to JK.EXE's (%d vs %u bytes)", n, size);
    }

    // ADDFORWARD (host -> existing peer) introducing a new machine
    dpn_TestBegin(kJk);
    m = load(dir, "h_05_2e.bin", &size, &ip);
    dpn_TestFeed(m, size, ip, 0);
    want = 0x38C4B182; wantName[0] = 0; inet_pton(AF_INET, "192.0.2.20", &wantIp); findCount = 0;
    dpn_TestPlayers(finder);
    CHECK(findCount == 1, "ADDFORWARD adds system player 38c4b182 at 192.0.2.20");

    // Host side: rebuild the vanilla host's replies from its own state and
    // compare byte for byte.
    {
        uint8_t ours[1024];
        const uint8_t* ref;
        uint32_t ip2;
        dps_session d;
        const char* nm = "deeznuts:JK1MP:m10.jkl";
        ref = load(dir, "c_00_01.bin", &size, &ip);
        memset(&d, 0, sizeof(d));
        for (int i = 0; nm[i]; i++) d.name[i] = (uint8_t)nm[i];
        d.flags = 0xC0; d.maxPlayers = 16;
        d.user[0] = 0x2D165A2C; d.user[1] = 0x90; d.user[2] = 0x0A; d.user[3] = 0x64;
        dpn_TestBegin(kJk);
        int n = dpn_TestHostEnumReply(ours, ref + DPN_TEST_DESC_GUID, 0x38C1B180, &d, 2);
        CHECK((uint32_t)n == size && !memcmp(ours, ref, size), "host ENUMSESSIONSREPLY identical to JK.EXE's (%d vs %u bytes)", n, size);

        ref = load(dir, "c_03_07.bin", &size, &ip);
        n = dpn_TestHostPlayerReply(ours, 0x38C4B182);
        CHECK((uint32_t)n == size && !memcmp(ours, ref, size), "host REQUESTPLAYERREPLY identical to JK.EXE's");

        ref = load(dir, "h_05_2e.bin", &size, &ip);
        inet_pton(AF_INET, "192.0.2.20", &ip2);
        n = dpn_TestHostAddForward(ours, 0x38C4B182, ip2);
        CHECK((uint32_t)n == size && !memcmp(ours, ref, size), "host ADDFORWARD identical to JK.EXE's");
    }

    // IPX: JK.EXE through IPXWrapper (host 192.0.2.10, joiner 192.0.2.20).
    // We stand in for the joiner, IPX address 00000001.020000000002:0400.
    {
        static const uint8_t joiner[12] = { 0,0,0,1, 0x02,0x00,0x00,0x00,0x00,0x02, 0x04,0x00 };
        static const uint8_t host[12] = { 0,0,0,1, 0x02,0x00,0x00,0x00,0x00,0x01, 0x04,0x00 };
        uint8_t sent[2048], addr[12], ack[28];
        uint32_t sentLen, hostIp, peerIp;
        uint16_t port, peerPort;
        int got;

        inet_pton(AF_INET, "192.0.2.10", &hostIp);
        dpn_TestIpxBegin(kJk, joiner);

        m = loadIpx(dir, "ipx_enumreply.bin", &size, &ip, &port);
        dpn_TestIpxPacket(m, size, ip, port, 0, NULL, NULL);
        e = dpn_TestEnumed(0);
        CHECK(e != NULL, "IPX: enum reply (no SP header) parsed");
        if (e)
        {
            str8(e->name, s, sizeof(s));
            CHECK(!strcmp(s, "deeznuts:JK1MP:m10.jkl") && e->flags == 0x4C0, "IPX: session '%s' flags 0x%x", s, e->flags);
        }

        // A guaranteed REQUESTPLAYERREPLY: acknowledged, delivered once.
        m = loadIpx(dir, "ipx_playerreply.bin", &size, &ip, &port);
        got = dpn_TestIpxPacket(m, size, ip, port, 0x07, sent, &sentLen);
        hex("706c617931000e00ff8b0439b04d584ebf392b394ab6bf7b00000000", ack);
        CHECK(got && dpn_TestReplyId() == 0x771B07C3, "IPX: PACKET2_DATA delivers REQUESTPLAYERREPLY (id %08x)", dpn_TestReplyId());
        CHECK(sentLen == 28 && !memcmp(sent, ack, 28), "IPX: PACKET2_ACK identical to JK.EXE's");
        got = dpn_TestIpxPacket(m, size, ip, port, 0x07, sent, &sentLen);
        CHECK(!got && sentLen == 28 && !memcmp(sent, ack, 28), "IPX: a resent copy is acknowledged again but not delivered twice");

        // SUPERENUMPLAYERSREPLY: everyone's SP data is a sockaddr_ipx.
        m = loadIpx(dir, "ipx_superenum.bin", &size, &ip, &port);
        got = dpn_TestIpxPacket(m, size, ip, port, 0x29, NULL, NULL);
        CHECK(got, "IPX: superenum delivered");
        want = 0x771B07C0; strcpy(wantName, "Annie"); findCount = 0;
        dpn_TestPlayers(ipxFinder);
        CHECK(findCount == 1 && dpn_TestIpxPeer(foundIp, addr, &peerIp, &peerPort) && !memcmp(addr, host, 12)
              && peerIp == hostIp && peerPort == 53355, "IPX: Annie (host) is at 00000001.020000000001:0400, 192.0.2.10:53355");
        want = 0x771B07C3; wantName[0] = 0; findCount = 0;
        dpn_TestPlayers(ipxFinder);
        CHECK(findCount == 1 && foundIp == 0, "IPX: our own system player's address (ours) stays 0");

        // The same message in two packets, last one first.
        {
            const uint8_t* inner = m + 27 + 48;
            uint32_t len = size - 27 - 48, cut = 100;
            uint8_t a[400], b[400];
            uint32_t na, nb;
            memcpy(a, m, 27 + 48); memcpy(b, m, 27 + 48);
            a[8 + 1 + 26] ^= 0x55; b[8 + 1 + 26] ^= 0x55;   // new message GUID
            memcpy(a + 27 + 48, inner, cut);
            memcpy(b + 27 + 48, inner + cut, len - cut);
            na = 27 + 48 + cut; nb = 27 + 48 + len - cut;
            a[25] = (uint8_t)((na - 27) >> 8); a[26] = (uint8_t)(na - 27);
            b[25] = (uint8_t)((nb - 27) >> 8); b[26] = (uint8_t)(nb - 27);
            // PacketIndex, DataSize, Offset, TotalPackets (MessageSize unchanged)
            a[27+24] = 0; a[27+28] = (uint8_t)cut; a[27+29] = 0; a[27+32] = 0; a[27+33] = 0; a[27+36] = 2;
            b[27+24] = 1; b[27+28] = (uint8_t)(len - cut); b[27+29] = (uint8_t)((len - cut) >> 8);
            b[27+32] = (uint8_t)cut; b[27+33] = 0; b[27+36] = 2;
            got = dpn_TestIpxPacket(b, nb, ip, port, 0x29, sent, &sentLen);
            CHECK(!got && sentLen == 28 && sent[24] == 1, "IPX: second packet of two acknowledged, held");
            got = dpn_TestIpxPacket(a, na, ip, port, 0x29, NULL, NULL);
            CHECK(got, "IPX: message reassembled from two packets");
        }

        // What we send must match JK.EXE's (no SP header; offsets from "play").
        {
            uint8_t ours[512];
            uint16_t name[] = { 'M','r',' ','C',0 };
            uint16_t pw[] = { 'p','a','s','s','w','o','r','d',0 };
            int n = dpn_TestBuildCreatePlayer(ours, 0x771B07C2, 0x771B07C3, name);
            m = loadIpx(dir, "ipx_createplayer.bin", &size, &ip, &port);
            CHECK((uint32_t)n - 20 == size - 27 - 48 && !memcmp(ours + 20, m + 27 + 48, (size_t)n - 20),
                  "IPX: CREATEPLAYER identical to JK.EXE's (%d vs %u bytes)", n - 20, size - 27 - 48);
            n = dpn_TestBuildAddForwardRequestPw(ours, 0x771B07C3, pw, 0x771A07C1);
            m = loadIpx(dir, "ipx_addfwdreq.bin", &size, &ip, &port);
            CHECK((uint32_t)n - 20 == size - 27 - 48 && !memcmp(ours + 20, m + 27 + 48, (size_t)n - 20),
                  "IPX: ADDFORWARDREQUEST identical to JK.EXE's (%d vs %u bytes)", n - 20, size - 27 - 48);
        }
    }

    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
