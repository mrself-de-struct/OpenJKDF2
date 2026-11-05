// Native DirectPlay 4 implementation of dplay_shim.h (no Windows, no Wine).
//
// Speaks the DirectPlay 4 Winsock wire protocol ([MC-DPL4CS]) directly over
// POSIX sockets so Linux/macOS builds of OpenJKDF2 can join games hosted by
// unmodified JK.EXE, and host games that unmodified JK.EXE can join. The host
// is DirectPlay's "name server": it answers session searches, assigns player
// IDs, introduces new machines to existing ones and keeps the session alive.
//
// Message layouts were checked against captures of real JK.EXE 1.0 sessions
// (DirectPlay dialect 14). Places marked [SPEC] follow the spec but haven't
// been seen in a capture yet. Set OPENJKDF2_DPLAY_LOG=1 to log every system
// message, OPENJKDF2_DPLAY_HOSTS="ip[,ip...]" to also send session searches
// directly to hosts that broadcasts can't reach.
//
// Single-threaded: all sockets are non-blocking and are serviced from
// dps_Receive()/dps_EnumSessions(), which the game calls every frame.
//
// A second connection, "DirectPlay IPX", plays with JK.EXE players who use
// IPXWrapper (github.com/solemnwarning/ipxwrapper) and DirectPlay's IPX
// connection. IPXWrapper carries IPX over UDP and replaces DirectPlay's IPX
// service provider with its own; we speak both directly, so nothing needs to
// be installed on our side. The DirectPlay messages are the same as over
// TCP/IP; only the transport underneath differs (see "IPX" below).

#if defined(PLATFORM_DPLAY) && defined(DPLAY_NATIVE)

#ifdef _WIN32
// Windows: the same code over Winsock. Microsoft's own DirectPlay is 32-bit
// only, so 64-bit Windows builds use this instead (and 32-bit ones can too).
#define FD_SETSIZE 128
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#ifndef MSG_NOSIGNAL
// macOS has no MSG_NOSIGNAL; sockets get SO_NOSIGPIPE instead (dpn_NoSigPipe).
#define MSG_NOSIGNAL 0
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DPS_BACKEND_PREFIX dpnative
#include "dplay_backend.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define DP_OK                   0
#define DPERR_GENERIC           ((long)0x80004005)
#define DPERR_OUTOFMEMORY       ((long)0x8007000E)
#define DPERR_NOSESSIONS        ((long)0x88770168)
#define DPERR_TIMEOUT           ((long)0x887700FA)  // reported as "not available" by the GUI
#define DPERR_UNAVAILABLE       ((long)0x887700FA)
#define DPERR_UNSUPPORTED       ((long)0x80004001)
#define DPERR_NOCONNECTION      ((long)0x887700AA)
#define DPERR_UNINITIALIZED     ((long)0x887707D0)
#define DPERR_ACCESSDENIED      ((long)0x88770014)
#define DPERR_INVALIDPLAYER     ((long)0x88770096)
#define DPERR_INVALIDPASSWORD   ((long)0x88770154)
#define DPERR_CANTCREATEPLAYER  ((long)0x8877003C)
#define DPERR_NONEWPLAYERS      ((long)0x8877014A)  // JK shows "game full"

#define DPN_TOKEN               0xFAB
#define DPN_VERSION             14          // dialect seen from JK.EXE on Win10/11 DirectPlay
#define DPN_ENUM_PORT           47624       // host listens here (UDP) for session searches
#define DPN_TCP_PORT_FIRST      2300
#define DPN_TCP_PORT_LAST       2349
#define DPN_UDP_PORT_FIRST      2350
#define DPN_UDP_PORT_LAST       2400
#define DPN_SPHDR               20          // size/token + sockaddr
#define DPN_HDR                 28          // + "play" + command + version
#define DPN_PACKED_FIXED        48          // DPLAYI_PACKEDPLAYER fixed part

// Commands
#define CMD_ENUMSESSIONSREPLY       0x01
#define CMD_ENUMSESSIONS            0x02
#define CMD_REQUESTPLAYERID         0x05
#define CMD_REQUESTPLAYERREPLY      0x07
#define CMD_CREATEPLAYER            0x08
#define CMD_DELETEPLAYER            0x0B
#define CMD_ADDFORWARDREQUEST       0x13
#define CMD_PING                    0x16    // [SPEC]
#define CMD_PINGREPLY               0x17    // [SPEC]
#define CMD_YOUAREDEAD              0x18    // [SPEC]
#define CMD_ADDFORWARDREPLY         0x24    // [SPEC] join refused (e.g. bad password)
#define CMD_SUPERENUMPLAYERSREPLY   0x29
#define CMD_ADDFORWARD              0x2E
#define CMD_ADDFORWARDACK           0x2F

// Player flags
#define DPF_SYSTEMPLAYER   0x1
#define DPF_NAMESERVER     0x2
#define DPF_INGROUP        0x4
#define DPF_LOCAL          0x8

#define DPN_MAX_PLAYERS   64
#define DPN_MAX_CONNS     64
#define DPN_MAX_ENUMED    32
#define DPN_ENUM_RESEND_MS    2000
#define DPN_ENUM_EXPIRE_MS    15000
#define DPN_JOIN_TIMEOUT_MS   10000
#define DPN_PING_PERIOD_MS    35000   // [SPEC] Ping timer period
#define DPN_PING_MAX_MISSED   8       // [SPEC] host considered gone after 8 unanswered pings
#define DPN_HOST_PING_MS      15000   // [WIRE] a JK.EXE host pings a silent machine every 15 s
#define DPN_HOST_ACK_WAIT_MS  2000    // max wait for ADDFORWARDACKs before sending the player list
#define DPN_MAX_SLOTS         256
#define DPN_CONNECT_TIMEOUT_MS 4000
#define DPN_BADADDR_MS        30000

// IPX the way IPXWrapper carries it: each IPX packet is one UDP datagram, a
// 27-byte header (u8 type, destination net(4) node(6) socket(2), source the
// same, u16 data size; big-endian) and the data. Machines listen on UDP 54792;
// a packet for an IPX address whose IP isn't known yet is broadcast to every
// interface's subnet, and replies come straight from the sender's UDP port.
// IPXWrapper only takes packets from its own subnets (a LAN, or a VPN such as
// Radmin). Its DirectPlay provider sends DirectPlay messages with no SP header,
// session searches to IPX socket 42367, and puts a sockaddr_ipx in each
// player's SP data. Guaranteed messages go through DirectPlay's own reliable
// layer, PACKET2_DATA/PACKET2_ACK ([WIRE], JK.EXE + IPXWrapper 2026-10-22).
#define DPN_IPX_UDP_PORT      54792
#define DPN_IPX_HDR           27
#define DPN_IPX_SOCKET        0x0400      // [WIRE] our IPX socket: the first one IPXWrapper hands out
#define DPN_IPX_DISC_SOCKET   42367       // [WIRE] where a host gets session searches
#define DPN_IPX_SPDATA        14          // sockaddr_ipx: family 6, net, node, socket
#define DPN_IPX_MAX_PEERS     64
#define DPN_IPX_MAX_PENDING   64
#define DPN_IPX_MAX_REASM     4
#define DPN_IPX_MAX_DONE      64
#define DPN_IPX_P2HDR         48          // "play" header + PACKET2_DATA fields
#define DPN_IPX_CHUNK         (1024 - DPN_IPX_P2HDR)  // the provider's dwMaxBufferSize is 1024
#define DPN_IPX_RESEND_MS     250         // [INFERRED] JK.EXE resent unacked ones after ~100 ms
#define DPN_IPX_RESEND_TRIES  24          // ~6 s, then the machine counts as unreachable
#define CMD_PACKET2_DATA      0x30
#define CMD_PACKET2_ACK       0x31

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef struct
{
    int      used;
    uint32_t id;
    uint32_t flags;
    uint32_t sysId;          // for normal players: owning system player
    uint16_t name[32];
    uint32_t ip;             // network byte order; from SP data or message source
    uint16_t tcpPort;        // host byte order
    uint16_t udpPort;
    uint64_t unreachableUntilMs; // skip TCP to this machine until then after a failed connect

    // host bookkeeping (system players of remote machines)
    uint64_t lastRxMs;
    uint64_t lastPingMs;
    int      missedPings;
    int      joinState;          // 0 none, 1 waiting for ADDFORWARDACKs, 2 joined
    int      acksPending;
    uint64_t joinStartMs;
} dpn_player;

typedef struct
{
    int      fd;
    int      outbound;       // we connected (keyed by ip:port)
    uint32_t ip;             // peer address (network order)
    uint16_t port;           // peer port (host order)
    uint8_t* buf;
    size_t   len, cap;

    // Outbound connections never block the game: connect() runs in the
    // background and messages queue in tx until the socket is writable.
    int      connecting;
    uint64_t connectStartMs;
    uint8_t* tx;
    size_t   txLen, txCap;
} dpn_conn;

typedef struct
{
    uint32_t ip;
    uint16_t port;
    uint64_t untilMs;
} dpn_badaddr;

typedef struct
{
    dps_session desc;
    uint32_t    reserved1;   // DPSESSIONDESC2.dwReserved1, echoed in ADDFORWARDREQUEST
    uint32_t    hostIp;
    uint16_t    hostPort;    // host's DirectPlay TCP port (from the reply header)
    uint64_t    lastSeenMs;
} dpn_enumed;

// A machine we talk to over IPX. Its handle (index + 1) stands in for an IPv4
// address everywhere else in this file, so the DirectPlay logic is shared.
typedef struct
{
    int      used;
    uint8_t  addr[12];       // IPX net (4), node (6), socket (2, network order)
    uint32_t ip;             // where its packets come from (network order); 0 until it sends us one
    uint16_t port;           // host order
    uint64_t lastRxMs;
} dpn_ipxpeer;

// One PACKET2_DATA packet waiting for its PACKET2_ACK.
typedef struct
{
    int      used;
    uint32_t to;
    uint8_t  guid[16];
    uint32_t index;
    int      tries;
    uint64_t nextMs;
    uint32_t len;
    uint8_t  pkt[DPN_IPX_P2HDR + DPN_IPX_CHUNK];
} dpn_ipxpending;

// A message arriving in several PACKET2_DATA packets.
typedef struct
{
    int      used;
    uint32_t from;
    uint8_t  guid[16];
    uint32_t size, total, got;
    uint8_t  have[80];
    uint8_t* buf;
    uint64_t startMs;
} dpn_ipxreasm;

typedef struct dpn_event
{
    struct dpn_event* next;
    int        recvType;     // DPS_RECV_DATA / DPS_RECV_SYS
    uint32_t   from;
    uint32_t   size;
    dps_sysmsg sys;
    uint8_t    data[];
} dpn_event;

static struct
{
    int      started;
    uint8_t  appGuid[16];
    int      log;

    int      tcpListen;
    uint16_t tcpPort;
    int      udp;
    uint16_t udpPort;
    int      enumSock;

    dpn_conn conns[DPN_MAX_CONNS];
    dpn_badaddr bad[16];         // recently unreachable ip:port, not retried for a while
    uint64_t startMs;

    // enumeration
    int        enumActive;
    uint64_t   lastEnumSendMs;
    dpn_enumed enumed[DPN_MAX_ENUMED];

    // session
    int         inSession;
    dps_session desc;
    uint32_t    hostIp;
    uint16_t    hostPort;
    uint32_t    localSysId;
    dpn_player  players[DPN_MAX_PLAYERS];

    // pending request/replies during synchronous calls
    int      waitCmd;
    int      gotReply;
    uint32_t replyId;
    long     replyResult;

    // hosting
    int      isHost;
    int      hostEnumSock;       // UDP 47624
    uint32_t key;                // DPSESSIONDESC2.dwReserved1; player IDs are XORed with it
    uint16_t slotUniq[DPN_MAX_SLOTS];
    uint8_t  slotUsed[DPN_MAX_SLOTS];
    uint32_t extraEnumHost;      // optional direct address for session searches (network order)

    // keepalive
    uint64_t lastHostRxMs;
    uint64_t lastPingMs;
    int      missedPings;

    dpn_event* evHead;
    dpn_event* evTail;

    // IPX (IPXWrapper-compatible) connection
    int      ipx;                // the IPX connection is selected
    int      ipxSock;            // UDP 54792
    uint16_t ipxUdpPort;
    uint8_t  ipxAddr[12];        // ours: net 1, a random node, DPN_IPX_SOCKET
    dpn_ipxpeer    ipxPeers[DPN_IPX_MAX_PEERS];
    dpn_ipxpending ipxPend[DPN_IPX_MAX_PENDING];
    dpn_ipxreasm   ipxReasm[DPN_IPX_MAX_REASM];
    uint8_t  ipxDone[DPN_IPX_MAX_DONE][20];   // GUID + sender of recent guaranteed messages
    int      ipxDoneNext;

    uint8_t  rxbuf[65536];
} N;

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

static uint64_t dpn_NowMs(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount();   // XP-compatible; wraps after 49 days
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

// --- socket portability (POSIX / Winsock) ---

static void dpn_CloseSock(int fd)
{
#ifdef _WIN32
    closesocket((SOCKET)fd);
#else
    close(fd);
#endif
}

static int dpn_SockErr(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static int dpn_WouldBlock(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static int dpn_ConnectPending(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS;
#endif
}

static const char* dpn_ErrStr(int err)
{
#ifdef _WIN32
    static char buf[32];
    snprintf(buf, sizeof(buf), "Winsock error %d", err);
    return buf;
#else
    return strerror(err);
#endif
}

#define DPN_PIN  1
#define DPN_POUT 2
#define DPN_PERR 4

typedef struct { int fd; int events, revents; } dpn_pfd;

// poll() for both platforms, on select(). (Windows reports a failed
// non-blocking connect through the "except" set.)
static int dpn_Poll(dpn_pfd* p, int n, int waitMs)
{
    fd_set rs, ws, es;
    struct timeval tv;
    int maxfd = 0, r, any = 0;
    FD_ZERO(&rs); FD_ZERO(&ws); FD_ZERO(&es);
    for (int i = 0; i < n; i++)
    {
        p[i].revents = 0;
        if (p[i].fd < 0) continue;
        if (p[i].events & DPN_PIN) FD_SET(p[i].fd, &rs);
        if (p[i].events & DPN_POUT) FD_SET(p[i].fd, &ws);
        FD_SET(p[i].fd, &es);
        if (p[i].fd > maxfd) maxfd = p[i].fd;
    }
    tv.tv_sec = waitMs / 1000;
    tv.tv_usec = (waitMs % 1000) * 1000;
    r = select(maxfd + 1, &rs, &ws, &es, &tv);
    if (r <= 0) return r;
    for (int i = 0; i < n; i++)
    {
        if (p[i].fd < 0) continue;
        if (FD_ISSET(p[i].fd, &rs)) p[i].revents |= DPN_PIN;
        if (FD_ISSET(p[i].fd, &ws)) p[i].revents |= DPN_POUT;
        if (FD_ISSET(p[i].fd, &es)) p[i].revents |= DPN_PERR;
        if (p[i].revents) any++;
    }
    return any;
}


static void dpn_Log(const char* fmt, ...)
{
    va_list ap;
    if (!N.log) return;
    va_start(ap, fmt);
    fprintf(stderr, "[dpnative %8.3f] ", (double)(dpn_NowMs() - N.startMs) / 1000.0);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void dpn_Warn(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[dpnative %8.3f] ", (double)(dpn_NowMs() - N.startMs) / 1000.0);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static const char* dpn_IpStr(uint32_t ipNet)
{
    static char buf[4][48];
    static int i = 0;
    char ip[INET_ADDRSTRLEN];
    struct in_addr a;
    const dpn_ipxpeer* peer = NULL;
    if (N.ipx && ipNet && ipNet <= DPN_IPX_MAX_PEERS && N.ipxPeers[ipNet - 1].used)
    {
        // An IPX peer handle: its node, and the IP it sends from.
        peer = &N.ipxPeers[ipNet - 1];
        ipNet = peer->ip;
    }
    a.s_addr = ipNet;
    i = (i + 1) & 3;
#ifdef _WIN32
    snprintf(ip, sizeof(ip), "%s", inet_ntoa(a));
#else
    inet_ntop(AF_INET, &a, ip, sizeof(ip));
#endif
    if (peer)
        snprintf(buf[i], sizeof(buf[i]), "ipx %02x%02x%02x%02x%02x%02x:%u (%s)", peer->addr[4], peer->addr[5],
                 peer->addr[6], peer->addr[7], peer->addr[8], peer->addr[9],
                 (unsigned)((peer->addr[10] << 8) | peer->addr[11]), peer->ip ? ip : "IP not known yet");
    else
        snprintf(buf[i], sizeof(buf[i]), "%s", ip);
    return buf[i];
}

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static size_t dpn_StrLen16(const uint16_t* s) { size_t n = 0; if (s) while (s[n]) n++; return n; }

// Reads a NUL-terminated UTF-16LE string at p (bounded by end).
static void dpn_ReadStr16(const uint8_t* p, const uint8_t* end, uint16_t* out, size_t outLen)
{
    size_t i = 0;
    if (!outLen) return;
    while (p + 1 < end && i + 1 < outLen)
    {
        uint16_t c = rd16(p);
        if (!c) break;
        out[i++] = c;
        p += 2;
    }
    out[i] = 0;
}

// Writing to a connection the other side closed raises SIGPIPE, which would
// kill the game. Linux avoids it per send (MSG_NOSIGNAL); macOS per socket.
static void dpn_NoSigPipe(int fd)
{
#if defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#else
    (void)fd;
#endif
}

static void dpn_SetNonBlock(int fd)
{
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket((SOCKET)fd, FIONBIO, &nb);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

#ifdef _WIN32
// Windows reports ICMP "port unreachable" from an earlier sendto() as an
// error on the next recvfrom() of a UDP socket; we don't want that.
static void dpn_NoUdpConnReset(int fd)
{
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl((SOCKET)fd, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &ret, NULL, NULL);
}
#endif

// ---------------------------------------------------------------------------
// Message building
// ---------------------------------------------------------------------------

// Writes the 28-byte DPSP_MSG_HEADER. 'size' is the full message size.
// The sockaddr advertises our DirectPlay TCP port; receivers combine it with
// the packet's source IP to reach us.
static void dpn_WriteHeader(uint8_t* m, uint32_t size, uint16_t cmd)
{
    memset(m, 0, DPN_HDR);
    wr32(m, (size & 0xFFFFF) | ((uint32_t)DPN_TOKEN << 20));
    wr16(m + 4, AF_INET);           // sin_family, little-endian as on Windows
    m[6] = (uint8_t)(N.tcpPort >> 8);  // sin_port, network order
    m[7] = (uint8_t)(N.tcpPort);
    memcpy(m + 20, "play", 4);
    wr16(m + 24, cmd);
    wr16(m + 26, DPN_VERSION);
}

static uint32_t dpn_SPDataLen(void) { return N.ipx ? DPN_IPX_SPDATA : 32; }

// SP data: two sockaddr_in (stream, datagram). JK.EXE leaves its own
// addresses 0; the host fills in a joiner's real IP when it forwards it.
// SP data for another machine (host side): its real address and ports.
// Over IPX it's a sockaddr_ipx, filled in for everyone ([WIRE]); ip is then a
// peer handle, 0 for ourselves.
static void dpn_WriteSPDataFor(uint8_t* p, uint32_t ip, uint16_t tcpPort, uint16_t udpPort)
{
    if (N.ipx)
    {
        const uint8_t* a = N.ipxAddr;
        if (ip && ip <= DPN_IPX_MAX_PEERS && N.ipxPeers[ip - 1].used) a = N.ipxPeers[ip - 1].addr;
        wr16(p, 6);                 // AF_IPX
        memcpy(p + 2, a, 12);
        return;
    }
    memset(p, 0, 32);
    wr16(p, AF_INET);
    p[2] = (uint8_t)(tcpPort >> 8); p[3] = (uint8_t)tcpPort;
    memcpy(p + 4, &ip, 4);
    wr16(p + 16, AF_INET);
    p[18] = (uint8_t)(udpPort >> 8); p[19] = (uint8_t)udpPort;
    memcpy(p + 20, &ip, 4);
}

static uint32_t dpn_WritePackedPlayerEx(uint8_t* p, uint32_t flags, uint32_t id, uint32_t sysId, const uint16_t* name,
                                        uint32_t ip, uint16_t tcpPort, uint16_t udpPort);

// DPLAYI_PACKEDPLAYER describing one of our own players. Returns its size.
static uint32_t dpn_WritePackedPlayer(uint8_t* p, uint32_t flags, uint32_t id, uint32_t sysId, const uint16_t* name)
{
    return dpn_WritePackedPlayerEx(p, flags, id, sysId, name, 0, N.tcpPort, N.udpPort);
}

static uint32_t dpn_WritePackedPlayerEx(uint8_t* p, uint32_t flags, uint32_t id, uint32_t sysId, const uint16_t* name,
                                        uint32_t ip, uint16_t tcpPort, uint16_t udpPort)
{
    uint32_t nameBytes = name ? (uint32_t)(dpn_StrLen16(name) + 1) * 2 : 0;
    uint32_t size = DPN_PACKED_FIXED + nameBytes + dpn_SPDataLen();
    uint8_t* q;

    memset(p, 0, DPN_PACKED_FIXED);
    wr32(p + 0, size);
    wr32(p + 4, flags);
    wr32(p + 8, id);
    wr32(p + 12, nameBytes);        // ShortNameLength
    wr32(p + 16, 0);                // LongNameLength
    wr32(p + 20, dpn_SPDataLen());  // ServiceProviderDataSize
    wr32(p + 24, 0);                // PlayerDataSize
    wr32(p + 28, 0);                // NumberOfPlayers
    wr32(p + 32, sysId);            // SystemPlayerID
    wr32(p + 36, DPN_PACKED_FIXED); // FixedSize
    wr32(p + 40, DPN_VERSION);      // PlayerVersion
    wr32(p + 44, 0);                // ParentID
    q = p + DPN_PACKED_FIXED;
    if (nameBytes)
    {
        for (uint32_t i = 0; i < nameBytes / 2; i++)
            wr16(q + i * 2, name[i]); // includes terminating 0 (name[len] == 0)
        q += nameBytes;
    }
    dpn_WriteSPDataFor(q, ip, tcpPort, udpPort);
    return size;
}

// ---------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------

static dpn_conn* dpn_AddConn(int fd, int outbound, uint32_t ip, uint16_t port)
{
    for (int i = 0; i < DPN_MAX_CONNS; i++)
    {
        if (N.conns[i].fd > 0) continue;
        memset(&N.conns[i], 0, sizeof(N.conns[i]));
        N.conns[i].fd = fd;
        N.conns[i].outbound = outbound;
        N.conns[i].ip = ip;
        N.conns[i].port = port;
        return &N.conns[i];
    }
    dpn_CloseSock(fd);
    return NULL;
}

static void dpn_CloseConn(dpn_conn* c)
{
    if (c->fd > 0) dpn_CloseSock(c->fd);
    free(c->buf);
    free(c->tx);
    memset(c, 0, sizeof(*c));
}

static int dpn_IsBadAddr(uint32_t ip, uint16_t port)
{
    uint64_t now = dpn_NowMs();
    for (int i = 0; i < 16; i++)
        if (N.bad[i].ip == ip && N.bad[i].port == port && now < N.bad[i].untilMs)
            return 1;
    return 0;
}

static void dpn_MarkBadAddr(uint32_t ip, uint16_t port)
{
    int slot = 0;
    for (int i = 0; i < 16; i++)
    {
        if (N.bad[i].ip == ip && N.bad[i].port == port) { slot = i; break; }
        if (N.bad[i].untilMs < N.bad[slot].untilMs) slot = i;
    }
    N.bad[slot].ip = ip;
    N.bad[slot].port = port;
    N.bad[slot].untilMs = dpn_NowMs() + DPN_BADADDR_MS;
}

// Writes as much queued data as the socket takes right now.
static int dpn_FlushTx(dpn_conn* c)
{
    while (c->txLen)
    {
        long n = send(c->fd, (const char*)c->tx, (int)c->txLen, MSG_NOSIGNAL);
        if (n > 0)
        {
            memmove(c->tx, c->tx + n, c->txLen - (size_t)n);
            c->txLen -= (size_t)n;
            continue;
        }
        if (n < 0 && dpn_WouldBlock()) return 0;
        return -1;
    }
    return 0;
}

static void dpn_FailConn(dpn_conn* c, const char* why)
{
    dpn_Warn("TCP to %s:%u failed (%s); not retrying for %d s\n", dpn_IpStr(c->ip), c->port, why, DPN_BADADDR_MS / 1000);
    dpn_MarkBadAddr(c->ip, c->port);
    dpn_CloseConn(c);
}

// Called from the pump when an outbound socket is writable (or errored).
static void dpn_ServiceOutbound(dpn_conn* c, int revents)
{
    if (c->connecting)
    {
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (char*)&err, &elen) < 0 || err || (revents & DPN_PERR))
        {
            dpn_FailConn(c, err ? dpn_ErrStr(err) : "connect error");
            return;
        }
        if (!(revents & DPN_POUT)) return;
        c->connecting = 0;
        dpn_Log("connected to %s:%u\n", dpn_IpStr(c->ip), c->port);
    }
    if (dpn_FlushTx(c) < 0) dpn_FailConn(c, "send error");
}

// Gives up on connections that never completed.
static void dpn_CheckConnectTimeouts(void)
{
    uint64_t now = dpn_NowMs();
    for (int i = 0; i < DPN_MAX_CONNS; i++)
        if (N.conns[i].fd > 0 && N.conns[i].connecting && now - N.conns[i].connectStartMs > DPN_CONNECT_TIMEOUT_MS)
            dpn_FailConn(&N.conns[i], "connect timed out");
}

static void dpn_CloseAllConns(void)
{
    for (int i = 0; i < DPN_MAX_CONNS; i++)
        if (N.conns[i].fd > 0)
            dpn_CloseConn(&N.conns[i]);
}

// Outbound connection to a peer's DirectPlay TCP port, reused across
// messages. Never waits: a new connection is started in the background.
static dpn_conn* dpn_GetOutbound(uint32_t ip, uint16_t port)
{
    struct sockaddr_in sa;
    dpn_conn* c;
    int fd;

    for (int i = 0; i < DPN_MAX_CONNS; i++)
        if (N.conns[i].fd > 0 && N.conns[i].outbound && N.conns[i].ip == ip && N.conns[i].port == port)
            return &N.conns[i];
    if (dpn_IsBadAddr(ip, port)) return NULL;

    fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    dpn_NoSigPipe(fd);
    dpn_SetNonBlock(fd);
    { int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one)); }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ip;
    sa.sin_port = htons(port);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0 && !dpn_ConnectPending())
    {
        dpn_Warn("connect %s:%u failed: %s\n", dpn_IpStr(ip), port, dpn_ErrStr(dpn_SockErr()));
        dpn_MarkBadAddr(ip, port);
        dpn_CloseSock(fd);
        return NULL;
    }
    c = dpn_AddConn(fd, 1, ip, port);
    if (!c) return NULL;
    c->connecting = 1;
    c->connectStartMs = dpn_NowMs();
    return c;
}

// Queues data on a connection and sends what it can immediately.
static int dpn_QueueSend(dpn_conn* c, const uint8_t* data, size_t len)
{
    if (c->txCap - c->txLen < len)
    {
        size_t ncap = c->txCap ? c->txCap : 4096;
        while (ncap - c->txLen < len) ncap *= 2;
        if (ncap > (1u << 20)) return -1;               // peer not reading; give up
        uint8_t* nt = (uint8_t*)realloc(c->tx, ncap);
        if (!nt) return -1;
        c->tx = nt;
        c->txCap = ncap;
    }
    memcpy(c->tx + c->txLen, data, len);
    c->txLen += len;
    return c->connecting ? 0 : dpn_FlushTx(c);
}

#ifdef DPLAY_NATIVE_TESTS
static int dpn_testCapture = 0;
static uint8_t dpn_testBuf[65536];
static uint32_t dpn_testLen = 0;
#endif

// ---------------------------------------------------------------------------
// IPX (IPXWrapper's encapsulation and DirectPlay provider)
// ---------------------------------------------------------------------------

static uint32_t dpn_Random32(void);
static void dpn_HandleSystem(const uint8_t* msg, uint32_t size, uint32_t srcIp);
static void dpn_OnGameData(const uint8_t* data, uint32_t n, uint32_t srcIp);

// Every interface's IPv4 broadcast address (Radmin's 26.255.255.255, a LAN's
// x.y.z.255, ...). Returns how many were written.
static int dpn_Broadcasts(uint32_t* out, int max)
{
    int n = 0;
#ifdef _WIN32
    INTERFACE_INFO ifs[32];
    DWORD got = 0;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return 0;
    if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, NULL, 0, ifs, sizeof(ifs), &got, NULL, NULL) == 0)
    {
        for (DWORD i = 0; i < got / sizeof(INTERFACE_INFO) && n < max; i++)
        {
            uint32_t addr = ifs[i].iiAddress.AddressIn.sin_addr.s_addr;
            uint32_t mask = ifs[i].iiNetmask.AddressIn.sin_addr.s_addr;
            if (!(ifs[i].iiFlags & IFF_UP) || !(ifs[i].iiFlags & IFF_BROADCAST) || (ifs[i].iiFlags & IFF_LOOPBACK)) continue;
            out[n++] = addr | ~mask;
        }
    }
    closesocket(s);
#else
    struct ifaddrs* ifs = NULL;
    if (getifaddrs(&ifs) == 0)
    {
        for (struct ifaddrs* i = ifs; i && n < max; i = i->ifa_next)
        {
            uint32_t bc = 0, addr;
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_BROADCAST) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            addr = ((struct sockaddr_in*)i->ifa_addr)->sin_addr.s_addr;
            if (i->ifa_broadaddr) bc = ((struct sockaddr_in*)i->ifa_broadaddr)->sin_addr.s_addr;
            // Altered: Radmin VPN's Linux interface has no broadcast address set
            // ("inet 26.x/8", no brd); glibc then reports the interface's own
            // address, so "broadcasts" went to this machine and never reached
            // Radmin. Work it out from the netmask, as Windows and IPXWrapper do.
            if ((!bc || bc == addr) && i->ifa_netmask)
                bc = addr | ~((struct sockaddr_in*)i->ifa_netmask)->sin_addr.s_addr;
            if (!bc || bc == 0xFFFFFFFFu) continue;
            for (int k = 0; k < n; k++) if (out[k] == bc) { bc = 0; break; }
            if (bc) out[n++] = bc;
        }
        freeifaddrs(ifs);
    }
#endif
    return n;
}

// The handle of the peer at this IPX address, added if new.
static uint32_t dpn_IpxPeer(const uint8_t addr[12])
{
    int slot = -1;
    for (int i = 0; i < DPN_IPX_MAX_PEERS; i++)
    {
        if (N.ipxPeers[i].used && !memcmp(N.ipxPeers[i].addr, addr, 12)) return (uint32_t)i + 1;
        if (slot < 0 && !N.ipxPeers[i].used) slot = i;
    }
    if (slot < 0)
    {
        slot = 0;   // full: reuse the one heard from longest ago
        for (int i = 1; i < DPN_IPX_MAX_PEERS; i++)
            if (N.ipxPeers[i].lastRxMs < N.ipxPeers[slot].lastRxMs) slot = i;
    }
    memset(&N.ipxPeers[slot], 0, sizeof(N.ipxPeers[slot]));
    N.ipxPeers[slot].used = 1;
    memcpy(N.ipxPeers[slot].addr, addr, 12);
    return (uint32_t)slot + 1;
}

static dpn_ipxpeer* dpn_IpxPeerOf(uint32_t handle)
{
    if (!handle || handle > DPN_IPX_MAX_PEERS || !N.ipxPeers[handle - 1].used) return NULL;
    return &N.ipxPeers[handle - 1];
}

// One IPX packet. With no IP for the destination yet, it's broadcast to every
// subnet, as IPXWrapper does; the right machine answers from its own address.
static int dpn_IpxSendRaw(const uint8_t dst[12], uint32_t ip, uint16_t port, const uint8_t* data, uint32_t len)
{
    uint8_t pkt[DPN_IPX_HDR + 8192];
    struct sockaddr_in sa;
    int sent = 0;

    if (len > 8192) return -1;      // IPXWrapper's MAX_DATA_SIZE
    pkt[0] = 0;                     // [WIRE] packet type 0
    memcpy(pkt + 1, dst, 12);
    memcpy(pkt + 13, N.ipxAddr, 12);
    pkt[25] = (uint8_t)(len >> 8);
    pkt[26] = (uint8_t)len;
    memcpy(pkt + DPN_IPX_HDR, data, len);
#ifdef DPLAY_NATIVE_TESTS
    if (dpn_testCapture)
    {
        if (len <= sizeof(dpn_testBuf)) { memcpy(dpn_testBuf, data, len); dpn_testLen = len; }
        return 0;
    }
#endif
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    if (ip)
    {
        sa.sin_addr.s_addr = ip;
        sa.sin_port = htons(port);
        return sendto(N.ipxSock, (const char*)pkt, (int)(DPN_IPX_HDR + len), 0, (struct sockaddr*)&sa, sizeof(sa)) < 0 ? -1 : 0;
    }
    {
        uint32_t bc[16];
        int n = dpn_Broadcasts(bc, 16);
        sa.sin_port = htons(DPN_IPX_UDP_PORT);
        for (int i = 0; i < n; i++)
        {
            sa.sin_addr.s_addr = bc[i];
            if (sendto(N.ipxSock, (const char*)pkt, (int)(DPN_IPX_HDR + len), 0, (struct sockaddr*)&sa, sizeof(sa)) >= 0) sent = 1;
        }
    }
    return sent ? 0 : -1;
}

// Unguaranteed: one packet to a peer.
static int dpn_IpxSendToPeer(uint32_t to, const uint8_t* data, uint32_t len)
{
    dpn_ipxpeer* p = dpn_IpxPeerOf(to);
    if (!p) return -1;
    return dpn_IpxSendRaw(p->addr, p->ip, p->port, data, len);
}

static void dpn_IpxWriteP2Header(uint8_t* m, uint16_t cmd)
{
    memcpy(m, "play", 4);
    wr16(m + 4, cmd);
    wr16(m + 6, DPN_VERSION);
}

// Guaranteed: DirectPlay's reliable layer. PACKET2_DATA = header, message
// GUID, PacketIndex, DataSize, Offset, TotalPackets, MessageSize,
// PackedOffset, data ([MC-DPL4CS] 2.2.43; every field as JK.EXE sends it).
// Resent until the matching PACKET2_ACK comes back.
static int dpn_IpxSendReliable(uint32_t to, const uint8_t* msg, uint32_t len)
{
    uint8_t guid[16];
    uint32_t total = len ? (len + DPN_IPX_CHUNK - 1) / DPN_IPX_CHUNK : 1;
    uint64_t now = dpn_NowMs();

    if (!dpn_IpxPeerOf(to) || total > 80) return -1;
    for (int i = 0; i < 16; i += 4) wr32(guid + i, dpn_Random32());
    for (uint32_t idx = 0; idx < total; idx++)
    {
        dpn_ipxpending* q = NULL;
        uint32_t off = idx * DPN_IPX_CHUNK;
        uint32_t n = len - off < DPN_IPX_CHUNK ? len - off : DPN_IPX_CHUNK;
        for (int i = 0; i < DPN_IPX_MAX_PENDING && !q; i++)
            if (!N.ipxPend[i].used) q = &N.ipxPend[i];
        if (!q)
        {
            dpn_Warn("ipx: too many unacknowledged messages, dropping one\n");
            return -1;
        }
        memset(q, 0, sizeof(*q) - sizeof(q->pkt));
        q->used = 1;
        q->to = to;
        memcpy(q->guid, guid, 16);
        q->index = idx;
        dpn_IpxWriteP2Header(q->pkt, CMD_PACKET2_DATA);
        memcpy(q->pkt + 8, guid, 16);
        wr32(q->pkt + 24, idx);
        wr32(q->pkt + 28, n);
        wr32(q->pkt + 32, off);
        wr32(q->pkt + 36, total);
        wr32(q->pkt + 40, len);
        wr32(q->pkt + 44, 0);
        memcpy(q->pkt + DPN_IPX_P2HDR, msg + off, n);
        q->len = DPN_IPX_P2HDR + n;
        q->tries = 1;
        q->nextMs = now + DPN_IPX_RESEND_MS;
        dpn_IpxSendToPeer(to, q->pkt, q->len);
    }
    return 0;
}

// A system message built with the TCP/IP header (DPN_HDR): searches, their
// replies and pings go as they are, everything else guaranteed ([WIRE]).
static int dpn_IpxSendSys(uint32_t to, const uint8_t* msg, uint32_t size)
{
    uint16_t cmd = rd16(msg + 24);
    if (size < DPN_HDR) return -1;
    if (dpn_IsBadAddr(to, 1)) return -1;
    if (N.log) dpn_Log("-> %s cmd=0x%02x size=%u\n", dpn_IpStr(to), cmd, size);
    if (cmd == CMD_ENUMSESSIONS || cmd == CMD_ENUMSESSIONSREPLY || cmd == CMD_PING || cmd == CMD_PINGREPLY)
        return dpn_IpxSendToPeer(to, msg + DPN_SPHDR, size - DPN_SPHDR);
    return dpn_IpxSendReliable(to, msg + DPN_SPHDR, size - DPN_SPHDR);
}

// Resends, and gives up on machines that stopped acknowledging.
static void dpn_IpxTick(void)
{
    uint64_t now = dpn_NowMs();
    for (int i = 0; i < DPN_IPX_MAX_PENDING; i++)
    {
        dpn_ipxpending* q = &N.ipxPend[i];
        if (!q->used || now < q->nextMs) continue;
        if (q->tries >= DPN_IPX_RESEND_TRIES)
        {
            uint32_t to = q->to;
            dpn_Warn("ipx: %s stopped acknowledging; not retrying for %d s\n", dpn_IpStr(to), DPN_BADADDR_MS / 1000);
            dpn_MarkBadAddr(to, 1);
            for (int k = 0; k < DPN_IPX_MAX_PENDING; k++)
                if (N.ipxPend[k].used && N.ipxPend[k].to == to) N.ipxPend[k].used = 0;
            continue;
        }
        // Resends also go to every subnet. IPXWrapper takes a packet for one
        // of its IPX addresses only from that interface's subnet, and when two
        // machines share two networks (a LAN and Radmin) it may answer us over
        // the other one; the copy that comes the right way gets through, and
        // its replies follow it ([WIRE], 2026-10-22).
        dpn_IpxSendToPeer(q->to, q->pkt, q->len);
        {
            dpn_ipxpeer* p = dpn_IpxPeerOf(q->to);
            if (p && p->ip) dpn_IpxSendRaw(p->addr, 0, 0, q->pkt, q->len);
        }
        q->tries++;
        q->nextMs = now + DPN_IPX_RESEND_MS;
    }
    for (int i = 0; i < DPN_IPX_MAX_REASM; i++)
        if (N.ipxReasm[i].used && now - N.ipxReasm[i].startMs > 30000)
        {
            free(N.ipxReasm[i].buf);
            memset(&N.ipxReasm[i], 0, sizeof(N.ipxReasm[i]));
        }
}

// Hands a system message that arrived without an SP header to the shared
// parser, behind a TCP/IP-style one (reply "port" 1: the peer's only socket).
static void dpn_IpxDeliverSys(const uint8_t* m, uint32_t n, uint32_t from)
{
    static uint8_t buf[DPN_SPHDR + 65536];
    if (n > 65536) return;
    memset(buf, 0, DPN_SPHDR);
    wr32(buf, ((DPN_SPHDR + n) & 0xFFFFF) | ((uint32_t)DPN_TOKEN << 20));
    wr16(buf + 4, AF_INET);
    buf[7] = 1;
    memcpy(buf + DPN_SPHDR, m, n);
    dpn_HandleSystem(buf, DPN_SPHDR + n, from);
}

static int dpn_IpxSeen(const uint8_t guid[16], uint32_t from)
{
    uint8_t key[20];
    memcpy(key, guid, 16);
    wr32(key + 16, from);
    for (int i = 0; i < DPN_IPX_MAX_DONE; i++)
        if (!memcmp(N.ipxDone[i], key, 20)) return 1;
    memcpy(N.ipxDone[N.ipxDoneNext], key, 20);
    N.ipxDoneNext = (N.ipxDoneNext + 1) % DPN_IPX_MAX_DONE;
    return 0;
}

static void dpn_IpxOnData(const uint8_t* m, uint32_t n, uint32_t from)
{
    uint8_t ack[28];
    const uint8_t* guid = m + 8;
    uint32_t idx, dsize, off, total, msize;
    dpn_ipxreasm* r = NULL;

    if (n < DPN_IPX_P2HDR) return;
    idx = rd32(m + 24); dsize = rd32(m + 28); off = rd32(m + 32); total = rd32(m + 36); msize = rd32(m + 40);
    if (dsize > n - DPN_IPX_P2HDR || !total || idx >= total || total > 80 || msize > 65536 || off + dsize > msize) return;

    // Every copy is acknowledged; JK.EXE resends until it hears one.
    dpn_IpxWriteP2Header(ack, CMD_PACKET2_ACK);
    memcpy(ack + 8, guid, 16);
    wr32(ack + 24, idx);
    dpn_IpxSendToPeer(from, ack, sizeof(ack));

    if (total == 1)
    {
        if (!dpn_IpxSeen(guid, from)) dpn_IpxDeliverSys(m + DPN_IPX_P2HDR, dsize, from);
        return;
    }
    for (int i = 0; i < DPN_IPX_MAX_REASM && !r; i++)
        if (N.ipxReasm[i].used && N.ipxReasm[i].from == from && !memcmp(N.ipxReasm[i].guid, guid, 16)) r = &N.ipxReasm[i];
    if (!r)
    {
        uint8_t key[20];
        memcpy(key, guid, 16);
        wr32(key + 16, from);
        for (int i = 0; i < DPN_IPX_MAX_DONE; i++)
            if (!memcmp(N.ipxDone[i], key, 20)) return;     // a late copy of one we finished
        for (int i = 0; i < DPN_IPX_MAX_REASM && !r; i++)
            if (!N.ipxReasm[i].used) r = &N.ipxReasm[i];
        if (!r) return;
        memset(r, 0, sizeof(*r));
        r->buf = (uint8_t*)malloc(msize ? msize : 1);
        if (!r->buf) return;
        r->used = 1;
        r->from = from;
        memcpy(r->guid, guid, 16);
        r->size = msize;
        r->total = total;
        r->startMs = dpn_NowMs();
    }
    if (r->size != msize || r->total != total || r->have[idx]) return;
    memcpy(r->buf + off, m + DPN_IPX_P2HDR, dsize);
    r->have[idx] = 1;
    if (++r->got < r->total) return;
    dpn_IpxSeen(guid, from);
    {
        uint8_t* buf = r->buf;
        uint32_t size = r->size;
        memset(r, 0, sizeof(*r));
        dpn_IpxDeliverSys(buf, size, from);
        free(buf);
    }
}

static void dpn_IpxOnAck(const uint8_t* m, uint32_t n, uint32_t from)
{
    if (n < 28) return;
    for (int i = 0; i < DPN_IPX_MAX_PENDING; i++)
    {
        dpn_ipxpending* q = &N.ipxPend[i];
        if (q->used && q->to == from && q->index == rd32(m + 24) && !memcmp(q->guid, m + 8, 16)) q->used = 0;
    }
}

// One received IPX datagram (header included) from ip:port.
static void dpn_IpxOnPacket(const uint8_t* pkt, uint32_t n, uint32_t ip, uint16_t port)
{
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    const uint8_t* dst = pkt + 1;
    const uint8_t* src = pkt + 13;
    const uint8_t* data = pkt + DPN_IPX_HDR;
    uint32_t len, from;
    uint16_t dsock;
    dpn_ipxpeer* peer;

    if (n < DPN_IPX_HDR) return;
    len = (uint32_t)((pkt[25] << 8) | pkt[26]);
    if (len != n - DPN_IPX_HDR) return;
    if (!memcmp(src + 4, N.ipxAddr + 4, 6)) return;            // our own broadcast
    if (!((src[10] << 8) | src[11])) return;                    // socket 0: IPXWrapper's own traffic
    dsock = (uint16_t)((dst[10] << 8) | dst[11]);
    if (memcmp(dst + 4, N.ipxAddr + 4, 6) && memcmp(dst + 4, bcast, 6)) return;   // for another machine
    if (dsock != DPN_IPX_SOCKET && !(dsock == DPN_IPX_DISC_SOCKET && N.isHost)) return;

    from = dpn_IpxPeer(src);
    peer = dpn_IpxPeerOf(from);
    peer->ip = ip;
    peer->port = port;
    peer->lastRxMs = dpn_NowMs();

    if (len >= 8 && !memcmp(data, "play", 4))
    {
        uint16_t cmd = rd16(data + 4);
        if (cmd == CMD_PACKET2_DATA) dpn_IpxOnData(data, len, from);
        else if (cmd == CMD_PACKET2_ACK) dpn_IpxOnAck(data, len, from);
        else dpn_IpxDeliverSys(data, len, from);
        return;
    }
    dpn_OnGameData(data, len, from);
}

static void dpn_ReadIpx(void)
{
    static uint8_t pkt[DPN_IPX_HDR + 8192];
    for (;;)
    {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        long n = recvfrom(N.ipxSock, (char*)pkt, sizeof(pkt), 0, (struct sockaddr*)&from, &flen);
        if (n <= 0) return;
        dpn_IpxOnPacket(pkt, (uint32_t)n, from.sin_addr.s_addr, ntohs(from.sin_port));
    }
}

static int dpn_SendTcp(uint32_t ip, uint16_t port, const uint8_t* msg, uint32_t size)
{
    if (N.ipx) return dpn_IpxSendSys(ip, msg, size);
#ifdef DPLAY_NATIVE_TESTS
    if (dpn_testCapture)
    {
        if (size <= sizeof(dpn_testBuf)) { memcpy(dpn_testBuf, msg, size); dpn_testLen = size; }
        return 0;
    }
#endif
    dpn_conn* c = dpn_GetOutbound(ip, port);
    if (!c) return -1;
    if (N.log)
        dpn_Log("-> %s:%u cmd=0x%02x size=%u%s\n", dpn_IpStr(ip), port, rd16(msg + 24), size, c->connecting ? " (queued)" : "");
    if (dpn_QueueSend(c, msg, size) < 0)
    {
        dpn_FailConn(c, "send error");
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Players
// ---------------------------------------------------------------------------

static dpn_player* dpn_FindPlayer(uint32_t id)
{
    for (int i = 0; i < DPN_MAX_PLAYERS; i++)
        if (N.players[i].used && N.players[i].id == id)
            return &N.players[i];
    return NULL;
}

static dpn_player* dpn_AddPlayer(uint32_t id)
{
    dpn_player* p = dpn_FindPlayer(id);
    if (p) return p;
    for (int i = 0; i < DPN_MAX_PLAYERS; i++)
    {
        if (N.players[i].used) continue;
        memset(&N.players[i], 0, sizeof(N.players[i]));
        N.players[i].used = 1;
        N.players[i].id = id;
        return &N.players[i];
    }
    return NULL;
}

// Where to reach a player: its own SP data, else its system player's.
static dpn_player* dpn_AddrOf(dpn_player* p)
{
    if (p && p->ip && p->udpPort) return p;
    if (p && !(p->flags & DPF_SYSTEMPLAYER)) return dpn_FindPlayer(p->sysId);
    return p;
}

static uint32_t dpn_IpxPeer(const uint8_t addr[12]);

// Applies SP data (two sockaddr_in). A zero address means "the sender".
static void dpn_ApplySPData(dpn_player* pl, const uint8_t* sp, uint32_t spLen, uint32_t srcIp)
{
    if (N.ipx)
    {
        // sockaddr_ipx. Ours stays 0, like our own TCP/IP SP data.
        static const uint8_t zero[10];
        if (spLen < DPN_IPX_SPDATA || rd16(sp) != 6) return;
        if (!memcmp(sp + 2, zero, 10)) pl->ip = srcIp;
        else if (!memcmp(sp + 2, N.ipxAddr, 12)) pl->ip = 0;
        else pl->ip = dpn_IpxPeer(sp + 2);
        pl->tcpPort = pl->udpPort = 1;      // one IPX socket carries everything
        return;
    }
    if (spLen < 32) return;
    uint32_t tcpIp, udpIp;
    memcpy(&tcpIp, sp + 4, 4);
    memcpy(&udpIp, sp + 20, 4);
    pl->tcpPort = (uint16_t)((sp[2] << 8) | sp[3]);
    pl->udpPort = (uint16_t)((sp[18] << 8) | sp[19]);
    pl->ip = udpIp ? udpIp : (tcpIp ? tcpIp : srcIp);
}

static void dpn_PushEvent(int recvType, uint32_t from, const void* data, uint32_t size, const dps_sysmsg* sys)
{
    dpn_event* e = (dpn_event*)malloc(sizeof(dpn_event) + size);
    if (!e) return;
    e->next = NULL;
    e->recvType = recvType;
    e->from = from;
    e->size = size;
    if (sys) e->sys = *sys; else memset(&e->sys, 0, sizeof(e->sys));
    if (size) memcpy(e->data, data, size);
    if (N.evTail) N.evTail->next = e; else N.evHead = e;
    N.evTail = e;
}

static void dpn_PushSys(int kind, uint32_t rawType, uint32_t playerId)
{
    dps_sysmsg s;
    memset(&s, 0, sizeof(s));
    s.kind = kind;
    s.rawType = rawType;
    s.playerId = playerId;
    dpn_PushEvent(DPS_RECV_SYS, 0, NULL, 0, &s);
}

static void dpn_FreeEvents(void)
{
    while (N.evHead)
    {
        dpn_event* e = N.evHead;
        N.evHead = e->next;
        free(e);
    }
    N.evTail = NULL;
}

// Parses a DPLAYI_PACKEDPLAYER into the player table.
static dpn_player* dpn_ParsePackedPlayer(const uint8_t* p, const uint8_t* end, uint32_t srcIp)
{
    if (end - p < DPN_PACKED_FIXED) return NULL;
    uint32_t size = rd32(p), flags = rd32(p + 4), id = rd32(p + 8);
    uint32_t snl = rd32(p + 12), lnl = rd32(p + 16), spl = rd32(p + 20);
    uint32_t sysId = rd32(p + 32), fixed = rd32(p + 36);
    const uint8_t* q = p + fixed;
    dpn_player* pl;

    if (fixed < DPN_PACKED_FIXED || size > (uint32_t)(end - p) || fixed + snl + lnl + spl > size)
        return NULL;
    pl = dpn_FindPlayer(id);
    if (pl && (pl->flags & DPF_LOCAL)) return NULL; // our own player echoed back; never overwrite it
    pl = dpn_AddPlayer(id);
    if (!pl) return NULL;
    pl->flags = flags & ~(uint32_t)DPF_LOCAL; // it's local to the sender, not to us
    pl->sysId = (flags & DPF_SYSTEMPLAYER) ? id : sysId;
    if (snl) dpn_ReadStr16(q, q + snl, pl->name, 32);
    q += snl + lnl;
    dpn_ApplySPData(pl, q, spl, srcIp);
    return pl;
}

// Parses one DPLAYI_SUPERPACKEDPLAYER. Returns pointer past it, or NULL.
static const uint8_t* dpn_ParseSuperPacked(const uint8_t* p, const uint8_t* end, uint32_t hostIp, int isGroup)
{
    if (end - p < 20) return NULL;
    uint32_t fixed = rd32(p);           // size of the fixed part after this field (16)
    uint32_t flags = rd32(p + 4), id = rd32(p + 8), mask = rd32(p + 12), verOrSys = rd32(p + 16);
    const uint8_t* q = p + 4 + fixed;
    uint16_t name[32] = { 0 };
    const uint8_t* sp = NULL;
    uint32_t spLen = 0;

    if (fixed < 16 || q > end) return NULL;

    // Variable-length "length" fields: 1, 2 or 4 bytes depending on 2-bit codes.
#define VLEN(code, out) do { \
        int c_ = (code); \
        if (c_ == 1) { if (q + 1 > end) return NULL; out = q[0]; q += 1; } \
        else if (c_ == 2) { if (q + 2 > end) return NULL; out = rd16(q); q += 2; } \
        else if (c_ == 3) { if (q + 4 > end) return NULL; out = rd32(q); q += 4; } \
        else out = 0; } while (0)

    if (mask & 0x1) // short name
    {
        dpn_ReadStr16(q, end, name, 32);
        while (q + 1 < end && rd16(q)) q += 2;
        q += 2;
    }
    if (mask & 0x2) // long name
    {
        while (q + 1 < end && rd16(q)) q += 2;
        q += 2;
    }
    {
        uint32_t pdLen;
        VLEN((mask >> 4) & 3, pdLen);   // [SPEC] player data
        q += pdLen;
        VLEN((mask >> 2) & 3, spLen);   // service provider data
        sp = q;
        q += spLen;
    }
    {
        uint32_t pc;
        VLEN((mask >> 6) & 3, pc);      // [SPEC] player IDs (groups)
        q += 4 * pc;
    }
    if (mask & 0x100) q += 4;           // [SPEC] parent ID
    {
        uint32_t sc;
        VLEN((mask >> 9) & 3, sc);      // [SPEC] shortcut IDs
        q += 4 * sc;
    }
#undef VLEN
    if (q > end) return NULL;

    if (!isGroup)
    {
        dpn_player* pl = dpn_AddPlayer(id);
        if (pl)
        {
            pl->flags = flags & ~(uint32_t)DPF_LOCAL;
            pl->sysId = (flags & DPF_SYSTEMPLAYER) ? id : verOrSys;
            memcpy(pl->name, name, sizeof(pl->name));
            if (sp) dpn_ApplySPData(pl, sp, spLen, hostIp);
            dpn_Log("   player %08x flags=%x sys=%08x %s:%u/%u\n", pl->id, pl->flags, pl->sysId,
                    dpn_IpStr(pl->ip), pl->tcpPort, pl->udpPort);
        }
    }
    return q;
}

// Copies a DPSESSIONDESC2 (80 bytes) into a dps_session (strings handled by caller).
static void dpn_ReadDesc(const uint8_t* d, dps_session* s, uint32_t* reserved1)
{
    memset(s, 0, sizeof(*s));
    s->flags = rd32(d + 4);
    memcpy(s->guidInstance, d + 8, 16);
    s->maxPlayers = rd32(d + 40);
    s->curPlayers = rd32(d + 44);
    if (reserved1) *reserved1 = rd32(d + 56);
    s->user[0] = rd32(d + 64);
    s->user[1] = rd32(d + 68);
    s->user[2] = rd32(d + 72);
    s->user[3] = rd32(d + 76);
}

// ---------------------------------------------------------------------------
// Incoming system messages
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Hosting (name server)
// ---------------------------------------------------------------------------

static uint32_t dpn_Random32(void)
{
    uint32_t v = 0;
#ifdef _WIN32
    // Session keys and instance GUIDs only need to be unpredictable enough not
    // to collide; mix a few changing values.
    static uint32_t s = 0;
    LARGE_INTEGER pc;
    QueryPerformanceCounter(&pc);
    s ^= (uint32_t)pc.QuadPart ^ (uint32_t)(pc.QuadPart >> 32) ^ GetTickCount() ^ (GetCurrentProcessId() << 16);
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    v = s * 2654435761u;
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f || fread(&v, 1, sizeof(v), f) != sizeof(v))
        v = (uint32_t)dpn_NowMs() * 2654435761u ^ (uint32_t)getpid();
    if (f) fclose(f);
#endif
    return v;
}

// [WIRE] IDs are key ^ ((uniqueness << 16) | slot), lowest free slot first
// (decoded from a JK.EXE host: key 0x38C1B180, its own IDs 0x38C0B180/81).
static uint32_t dpn_HostAllocId(void)
{
    for (int slot = 0; slot < DPN_MAX_SLOTS; slot++)
    {
        if (N.slotUsed[slot]) continue;
        N.slotUsed[slot] = 1;
        N.slotUniq[slot]++;
        return N.key ^ (((uint32_t)N.slotUniq[slot] << 16) | (uint32_t)slot);
    }
    return 0;
}

static void dpn_HostFreeId(uint32_t id)
{
    uint32_t slot = (id ^ N.key) & 0xFFFF;
    if (slot < DPN_MAX_SLOTS) N.slotUsed[slot] = 0;
}

static uint32_t dpn_CountGamePlayers(void)
{
    uint32_t n = 0;
    for (int i = 0; i < DPN_MAX_PLAYERS; i++)
        if (N.players[i].used && !(N.players[i].flags & DPF_SYSTEMPLAYER)) n++;
    return n;
}

// DPSESSIONDESC2 as it appears on the wire (string pointers are 0).
static void dpn_WriteDesc(uint8_t* d)
{
    memset(d, 0, 80);
    wr32(d + 0, 80);
    wr32(d + 4, N.desc.flags);
    memcpy(d + 8, N.desc.guidInstance, 16);
    memcpy(d + 24, N.appGuid, 16);
    wr32(d + 40, N.desc.maxPlayers);
    wr32(d + 44, dpn_CountGamePlayers());
    wr32(d + 56, N.key);
    for (int i = 0; i < 4; i++) wr32(d + 64 + 4 * i, N.desc.user[i]);
}

static void dpn_HostSendEnumReply(uint32_t ip, uint16_t port)
{
    uint8_t m[DPN_HDR + 80 + 4 + DPS_NAME_MAX * 2];
    uint32_t nameBytes = (uint32_t)(dpn_StrLen16(N.desc.name) + 1) * 2;
    uint32_t size = DPN_HDR + 80 + 4 + nameBytes;
    dpn_WriteHeader(m, size, CMD_ENUMSESSIONSREPLY);
    dpn_WriteDesc(m + DPN_HDR);
    wr32(m + DPN_HDR + 80, 8 + 80 + 4);   // NameOffset, from "play"
    for (uint32_t i = 0; i < nameBytes / 2; i++) wr16(m + DPN_HDR + 84 + i * 2, N.desc.name[i]);
    dpn_SendTcp(ip, port, m, size);
}

static void dpn_HostSendPlayerReply(uint32_t ip, uint16_t port, uint32_t id)
{
    uint8_t m[DPN_HDR + 40];
    dpn_WriteHeader(m, sizeof(m), CMD_REQUESTPLAYERREPLY);
    memset(m + DPN_HDR, 0, 40);   // security descriptor, SSPI/CAPI offsets, result = DP_OK
    wr32(m + DPN_HDR, id);
    dpn_SendTcp(ip, port, m, sizeof(m));
}

static void dpn_HostSendError(uint32_t ip, uint16_t port, long err)
{
    uint8_t m[DPN_HDR + 4];   // [SPEC] DPSP_MSG_ADDFORWARDREPLY: Error
    dpn_WriteHeader(m, sizeof(m), CMD_ADDFORWARDREPLY);
    wr32(m + DPN_HDR, (uint32_t)err);
    dpn_SendTcp(ip, port, m, sizeof(m));
}

// One DPLAYI_SUPERPACKEDPLAYER. Flags as JK.EXE sends them: everything in the
// session carries 0x4, the host's own players also carry LOCAL.
static uint32_t dpn_WriteSuperPacked(uint8_t* p, const dpn_player* pl)
{
    uint8_t* q = p;
    uint32_t flags = (pl->flags & ~(uint32_t)DPF_LOCAL) | DPF_INGROUP;
    int mine = (pl->flags & DPF_LOCAL) != 0;
    int hasName = pl->name[0] != 0;

    if (mine) flags |= DPF_LOCAL;
    wr32(q, 16); q += 4;
    wr32(q, flags); q += 4;
    wr32(q, pl->id); q += 4;
    wr32(q, (hasName ? 0x1 : 0) | 0x4); q += 4;       // short name? + 1-byte SP data length
    wr32(q, (pl->flags & DPF_SYSTEMPLAYER) ? DPN_VERSION : pl->sysId); q += 4;
    if (hasName)
    {
        size_t n = dpn_StrLen16(pl->name);
        for (size_t i = 0; i <= n; i++) { wr16(q, pl->name[i]); q += 2; }
    }
    *q++ = (uint8_t)dpn_SPDataLen();
    {
        const dpn_player* a = (pl->flags & DPF_SYSTEMPLAYER) ? pl : dpn_FindPlayer(pl->sysId);
        if (!a) a = pl;
        if (mine) dpn_WriteSPDataFor(q, 0, N.tcpPort, N.udpPort);
        else      dpn_WriteSPDataFor(q, a->ip, a->tcpPort, a->udpPort);
    }
    q += dpn_SPDataLen();
    return (uint32_t)(q - p);
}

static void dpn_HostSendSuperEnum(dpn_player* joiner)
{
    uint8_t* m = (uint8_t*)malloc(65536);
    uint32_t nameBytes = (uint32_t)(dpn_StrLen16(N.desc.name) + 1) * 2;
    uint32_t pc = 0, off;
    uint8_t* q;

    if (!m) return;
    // body: PlayerCount, GroupCount, PackedOffset, ShortcutCount,
    //       DescriptionOffset, NameOffset, PasswordOffset   (offsets from "play")
    off = 8 + 28;
    dpn_WriteDesc(m + DPN_SPHDR + off);
    off += 80;
    for (uint32_t i = 0; i < nameBytes / 2; i++) wr16(m + DPN_SPHDR + off + i * 2, N.desc.name[i]);
    wr32(m + DPN_HDR + 16, 8 + 28);
    wr32(m + DPN_HDR + 20, off);
    off += nameBytes;
    wr32(m + DPN_HDR + 8, off);

    q = m + DPN_SPHDR + off;
    for (int pass = 0; pass < 2; pass++)            // system players first, like JK.EXE
        for (int i = 0; i < DPN_MAX_PLAYERS; i++)
        {
            dpn_player* pl = &N.players[i];
            if (!pl->used || ((pl->flags & DPF_SYSTEMPLAYER) != 0) != (pass == 0)) continue;
            if ((pl->flags & DPF_SYSTEMPLAYER) && pl->joinState == 1 && pl != joiner) continue;
            q += dpn_WriteSuperPacked(q, pl);
            pc++;
        }
    wr32(m + DPN_HDR + 0, pc);
    wr32(m + DPN_HDR + 4, 0);
    wr32(m + DPN_HDR + 12, 0);
    wr32(m + DPN_HDR + 24, 0);
    dpn_WriteHeader(m, (uint32_t)(q - m), CMD_SUPERENUMPLAYERSREPLY);  // only touches the first 28 bytes
    dpn_SendTcp(joiner->ip, joiner->tcpPort, m, (uint32_t)(q - m));
    joiner->joinState = 2;
    joiner->lastRxMs = dpn_NowMs();
    free(m);
    dpn_Log("host: %s joined as system player %08x (%u entries sent)\n", dpn_IpStr(joiner->ip), joiner->id, pc);
}

// Removes a remote machine and its players, tells everyone else.
static void dpn_SendDeletePlayer(uint32_t id);
static void dpn_HostDropMachine(dpn_player* sys, const char* why)
{
    uint32_t sysId = sys->id;
    dpn_Warn("host: dropping %s (%s)\n", dpn_IpStr(sys->ip), why);
    sys->unreachableUntilMs = ~(uint64_t)0;   // don't try to tell the machine we're dropping
    for (int i = 0; i < DPN_MAX_PLAYERS; i++)
    {
        dpn_player* p = &N.players[i];
        if (!p->used || (p->flags & DPF_SYSTEMPLAYER) || p->sysId != sysId) continue;
        uint32_t id = p->id;
        p->used = 0;
        dpn_SendDeletePlayer(id);
        dpn_HostFreeId(id);
        dpn_PushSys(DPS_SYS_PLAYERDESTROYED, 0x0005, id);
    }
    sys->used = 0;
    sys->unreachableUntilMs = ~(uint64_t)0;
    dpn_SendDeletePlayer(sysId);
    dpn_HostFreeId(sysId);
}

static void dpn_HostTick(void)
{
    uint64_t now = dpn_NowMs();
    for (int i = 0; i < DPN_MAX_PLAYERS; i++)
    {
        dpn_player* p = &N.players[i];
        if (!p->used || !(p->flags & DPF_SYSTEMPLAYER) || (p->flags & DPF_LOCAL)) continue;

        if (p->joinState == 1)
        {
            if (!p->acksPending || now - p->joinStartMs > DPN_HOST_ACK_WAIT_MS)
                dpn_HostSendSuperEnum(p);
            continue;
        }
        if (p->joinState == 0)
        {
            // Got an ID but never sent ADDFORWARDREQUEST: give the slot back.
            if (now - p->joinStartMs > DPN_JOIN_TIMEOUT_MS) { dpn_HostFreeId(p->id); p->used = 0; }
            continue;
        }
        if (now - p->lastRxMs < DPN_HOST_PING_MS || now - p->lastPingMs < DPN_HOST_PING_MS) continue;
        if (p->missedPings >= DPN_PING_MAX_MISSED) { dpn_HostDropMachine(p, "stopped answering pings"); continue; }

        // [WIRE] JK.EXE's host pings over UDP, to the machine's datagram port.
        uint8_t m[DPN_HDR + 8];
        struct sockaddr_in sa;
        dpn_WriteHeader(m, sizeof(m), CMD_PING);
        wr32(m + DPN_HDR, N.localSysId);
        wr32(m + DPN_HDR + 4, (uint32_t)now);
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = p->ip;
        sa.sin_port = htons(p->udpPort);
        if (N.ipx) dpn_IpxSendToPeer(p->ip, m + DPN_SPHDR, sizeof(m) - DPN_SPHDR);
        else sendto(N.udp, (const char*)m, sizeof(m), 0, (struct sockaddr*)&sa, sizeof(sa));
        p->lastPingMs = now;
        p->missedPings++;
    }
}

// Marks traffic from a remote machine (keeps the host from pinging/dropping it).
static void dpn_NoteRx(uint32_t srcIp)
{
    uint64_t now = dpn_NowMs();
    for (int i = 0; i < DPN_MAX_PLAYERS; i++)
    {
        dpn_player* p = &N.players[i];
        if (p->used && (p->flags & DPF_SYSTEMPLAYER) && p->ip == srcIp)
        {
            p->lastRxMs = now;
            p->missedPings = 0;
        }
    }
}

static void dpn_SendPingReply(uint32_t toIp, uint16_t toPort, uint32_t tick);

static void dpn_SendPingReply(uint32_t toIp, uint16_t toPort, uint32_t tick)
{
    uint8_t m[DPN_HDR + 8];
    dpn_WriteHeader(m, sizeof(m), CMD_PINGREPLY);
    wr32(m + DPN_HDR, N.localSysId);
    wr32(m + DPN_HDR + 4, tick);
    dpn_SendTcp(toIp, toPort, m, sizeof(m));
}

// msg points at the start of the 20-byte SP header; size is the full size.
static void dpn_HandleSystem(const uint8_t* msg, uint32_t size, uint32_t srcIp)
{
    const uint8_t* base = msg + DPN_SPHDR;   // offsets in messages are relative to "play"
    const uint8_t* end = msg + size;
    const uint8_t* b = msg + DPN_HDR;        // message body
    uint16_t cmd = rd16(msg + 24);
    uint16_t replyPort = (uint16_t)((msg[6] << 8) | msg[7]);

    if (size < DPN_HDR) return;
    if (srcIp == N.hostIp) N.lastHostRxMs = dpn_NowMs();
    if (N.isHost) dpn_NoteRx(srcIp);
    dpn_Log("<- %s cmd=0x%02x size=%u\n", dpn_IpStr(srcIp), cmd, size);

    switch (cmd)
    {
        case CMD_ENUMSESSIONSREPLY:
        {
            dps_session s;
            uint32_t res1, nameOff;
            dpn_enumed* slot = NULL;
            if (size < DPN_HDR + 80 + 4 || !N.enumActive) break;
            dpn_ReadDesc(b, &s, &res1);
            if (memcmp(b + 24, N.appGuid, 16)) break;           // not JK
            nameOff = rd32(b + 80);
            if (nameOff && base + nameOff < end)
                dpn_ReadStr16(base + nameOff, end, s.name, DPS_NAME_MAX);

            for (int i = 0; i < DPN_MAX_ENUMED; i++)
                if (N.enumed[i].lastSeenMs && !memcmp(N.enumed[i].desc.guidInstance, s.guidInstance, 16))
                    slot = &N.enumed[i];
            for (int i = 0; !slot && i < DPN_MAX_ENUMED; i++)
                if (!N.enumed[i].lastSeenMs) slot = &N.enumed[i];
            if (!slot) break;
            slot->desc = s;
            slot->reserved1 = res1;
            slot->hostIp = srcIp;
            slot->hostPort = replyPort ? replyPort : DPN_TCP_PORT_FIRST;
            slot->lastSeenMs = dpn_NowMs();
            break;
        }

        case CMD_REQUESTPLAYERREPLY:
            if (N.waitCmd == CMD_REQUESTPLAYERREPLY && size >= DPN_HDR + 40)
            {
                N.replyId = rd32(b);
                N.replyResult = (long)(int32_t)rd32(b + 4 + 24 + 8);
                N.gotReply = 1;
            }
            break;

        case CMD_ADDFORWARDREPLY:   // [SPEC] join refused
            if (N.waitCmd == CMD_SUPERENUMPLAYERSREPLY && size >= DPN_HDR + 4)
            {
                N.replyResult = (long)(int32_t)rd32(b);
                if (!N.replyResult) N.replyResult = DPERR_ACCESSDENIED;
                N.gotReply = 1;
            }
            break;

        case CMD_SUPERENUMPLAYERSREPLY:
        {
            if (N.waitCmd != CMD_SUPERENUMPLAYERSREPLY || size < DPN_HDR + 28) break;
            uint32_t pc = rd32(b), gc = rd32(b + 4), packedOff = rd32(b + 8);
            uint32_t descOff = rd32(b + 16), nameOff = rd32(b + 20), pwOff = rd32(b + 24);
            const uint8_t* q;
            (void)pwOff;

            if (descOff && base + descOff + 80 <= end)
                dpn_ReadDesc(base + descOff, &N.desc, NULL);
            if (nameOff && base + nameOff < end)
                dpn_ReadStr16(base + nameOff, end, N.desc.name, DPS_NAME_MAX);

            dpn_Log("superenum: %u players, %u groups\n", pc, gc);
            q = base + packedOff;
            for (uint32_t i = 0; i < pc + gc && q && q < end; i++)
                q = dpn_ParseSuperPacked(q, end, srcIp, i >= pc);
            if (!q) dpn_Warn("superenum: malformed player list\n");

            N.replyResult = DP_OK;
            N.gotReply = 1;
            break;
        }

        case CMD_ADDFORWARD:
        {
            // Host introduces a new machine (its system player) to us.
            if (size < DPN_HDR + 20) break;
            uint32_t id = rd32(b + 4), createOff = rd32(b + 12);
            dpn_player* pl = dpn_ParsePackedPlayer(base + createOff, end, srcIp);
            if (pl) dpn_Log("addforward: system player %08x at %s\n", id, dpn_IpStr(pl->ip));
            {
                uint8_t m[DPN_HDR + 4];
                dpn_WriteHeader(m, sizeof(m), CMD_ADDFORWARDACK);
                wr32(m + DPN_HDR, id);
                dpn_SendTcp(srcIp, replyPort ? replyPort : DPN_TCP_PORT_FIRST, m, sizeof(m));
            }
            break;
        }

        case CMD_CREATEPLAYER:
        {
            if (size < DPN_HDR + 20) break;
            uint32_t createOff = rd32(b + 12);
            dpn_player* pl = dpn_ParsePackedPlayer(base + createOff, end, srcIp);
            if (pl && !(pl->flags & DPF_SYSTEMPLAYER))
                dpn_PushSys(DPS_SYS_PLAYERCREATED, 0x0003, pl->id);
            break;
        }

        case CMD_DELETEPLAYER:
        {
            if (size < DPN_HDR + 8) break;
            uint32_t id = rd32(b + 4);
            dpn_player* pl = dpn_FindPlayer(id);
            if (!pl) break;
            int wasSystem = (pl->flags & DPF_SYSTEMPLAYER) != 0;
            int wasHost = wasSystem && (pl->flags & DPF_NAMESERVER);
            if (!wasSystem)
                dpn_PushSys(DPS_SYS_PLAYERDESTROYED, 0x0005, id);
            pl->used = 0;
            if (N.isHost) dpn_HostFreeId(id);
            if (wasHost)
                dpn_PushSys(DPS_SYS_SESSIONLOST, 0x0031, 0);
            break;
        }

        case CMD_PING:
            if (size >= DPN_HDR + 8)
                dpn_SendPingReply(srcIp, replyPort ? replyPort : DPN_TCP_PORT_FIRST, rd32(b + 4));
            break;

        case CMD_PINGREPLY:
            N.missedPings = 0;
            break;

        case CMD_YOUAREDEAD:
            dpn_Warn("host says we are no longer in the session\n");
            if (N.inSession) dpn_PushSys(DPS_SYS_SESSIONLOST, 0x0031, 0);
            break;

        case CMD_ENUMSESSIONS:
        {
            // body: guidApplication, PasswordOffset, Flags
            if (!N.isHost || size < DPN_HDR + 24) break;
            if (memcmp(b, N.appGuid, 16)) break;
            uint32_t flags = rd32(b + 20);
            if ((N.desc.flags & DPS_SESSION_PASSWORDREQUIRED) && !(flags & 0x40)) break; // searcher hides passworded games
            dpn_HostSendEnumReply(srcIp, replyPort ? replyPort : DPN_TCP_PORT_FIRST);
            break;
        }

        case CMD_REQUESTPLAYERID:
        {
            if (!N.isHost || size < DPN_HDR + 4) break;
            uint32_t flags = rd32(b);
            uint32_t id = dpn_HostAllocId();
            if (!id) break;
            if (flags & DPF_SYSTEMPLAYER)
            {
                dpn_player* pl = dpn_AddPlayer(id);
                if (!pl) { dpn_HostFreeId(id); break; }
                pl->flags = DPF_SYSTEMPLAYER;
                pl->sysId = id;
                pl->ip = srcIp;
                pl->tcpPort = replyPort ? replyPort : DPN_TCP_PORT_FIRST;
                pl->joinState = 0;
                pl->joinStartMs = dpn_NowMs();
                pl->lastRxMs = pl->joinStartMs;
            }
            dpn_HostSendPlayerReply(srcIp, replyPort ? replyPort : DPN_TCP_PORT_FIRST, id);
            break;
        }

        case CMD_ADDFORWARDREQUEST:
        {
            if (!N.isHost || size < DPN_HDR + 20) break;
            uint32_t id = rd32(b + 4), createOff = rd32(b + 12), pwOff = rd32(b + 16);
            uint16_t pw[DPS_PASSWORD_MAX] = { 0 };
            uint16_t port = replyPort ? replyPort : DPN_TCP_PORT_FIRST;
            dpn_player* pl = dpn_FindPlayer(id);

            if (pwOff && base + pwOff < end) dpn_ReadStr16(base + pwOff, end, pw, DPS_PASSWORD_MAX);
            if (!pl || !(pl->flags & DPF_SYSTEMPLAYER)) { dpn_HostSendError(srcIp, port, DPERR_CANTCREATEPLAYER); break; }
            if ((N.desc.flags & DPS_SESSION_PASSWORDREQUIRED) &&
                (dpn_StrLen16(pw) != dpn_StrLen16(N.desc.password) || memcmp(pw, N.desc.password, dpn_StrLen16(pw) * 2)))
            {
                dpn_Log("host: %s gave the wrong password\n", dpn_IpStr(srcIp));
                dpn_HostSendError(srcIp, port, DPERR_INVALIDPASSWORD);
                dpn_HostFreeId(id); pl->used = 0;
                break;
            }
            if ((N.desc.flags & DPS_SESSION_JOINDISABLED) || dpn_CountGamePlayers() >= N.desc.maxPlayers)
            {
                dpn_HostSendError(srcIp, port, DPERR_NONEWPLAYERS);
                dpn_HostFreeId(id); pl->used = 0;
                break;
            }
            if (base + createOff < end) dpn_ParsePackedPlayer(base + createOff, end, srcIp);
            pl->ip = srcIp;     // SP data addresses are 0; the source is the machine
            if (!pl->tcpPort) pl->tcpPort = port;

            // [WIRE] Introduce the new machine to everyone already in the game
            // (ADDFORWARD), wait for their ADDFORWARDACKs, then send it the
            // full player list -- the order a JK.EXE host uses.
            {
                uint8_t m[DPN_HDR + 20 + DPN_PACKED_FIXED + 32];
                uint32_t packed = dpn_WritePackedPlayerEx(m + DPN_HDR + 20, DPF_SYSTEMPLAYER | DPF_INGROUP, id, id, NULL,
                                                          pl->ip, pl->tcpPort, pl->udpPort);
                uint32_t msize = DPN_HDR + 20 + packed;
                dpn_WriteHeader(m, msize, CMD_ADDFORWARD);
                wr32(m + DPN_HDR + 0, 0);
                wr32(m + DPN_HDR + 4, id);
                wr32(m + DPN_HDR + 8, 0);
                wr32(m + DPN_HDR + 12, 0x1C);
                wr32(m + DPN_HDR + 16, 0);
                pl->acksPending = 0;
                for (int i = 0; i < DPN_MAX_PLAYERS; i++)
                {
                    dpn_player* o = &N.players[i];
                    if (!o->used || o == pl || !(o->flags & DPF_SYSTEMPLAYER) || (o->flags & DPF_LOCAL) || o->joinState != 2) continue;
                    if (dpn_SendTcp(o->ip, o->tcpPort, m, msize) == 0) pl->acksPending++;
                }
            }
            pl->joinState = 1;
            pl->joinStartMs = dpn_NowMs();
            if (!pl->acksPending) dpn_HostSendSuperEnum(pl);
            break;
        }

        case CMD_ADDFORWARDACK:
        {
            if (!N.isHost || size < DPN_HDR + 4) break;
            dpn_player* pl = dpn_FindPlayer(rd32(b));
            if (pl && pl->joinState == 1 && pl->acksPending > 0 && --pl->acksPending == 0)
                dpn_HostSendSuperEnum(pl);
            break;
        }

        default:
            if (N.log)
            {
                dpn_Log("   unhandled cmd 0x%02x, body:", cmd);
                for (uint32_t i = DPN_HDR; i < size && i < DPN_HDR + 64; i++)
                    fprintf(stderr, " %02x", msg[i]);
                fprintf(stderr, "\n");
            }
            break;
    }
}

// Extracts complete messages from a TCP connection's buffer.
static void dpn_DrainConn(dpn_conn* c)
{
    for (;;)
    {
        if (c->len < 4) return;
        uint32_t st = rd32(c->buf);
        uint32_t size = st & 0xFFFFF;
        if ((st >> 20) != DPN_TOKEN || size < DPN_HDR || size > 0x10000)
        {
            dpn_Warn("bad framing from %s (0x%08x), dropping connection\n", dpn_IpStr(c->ip), st);
            dpn_CloseConn(c);
            return;
        }
        if (c->len < size) return;
        if (!memcmp(c->buf + 20, "play", 4))
            dpn_HandleSystem(c->buf, size, c->ip);
        if (c->fd <= 0) return; // closed during handling
        memmove(c->buf, c->buf + size, c->len - size);
        c->len -= size;
    }
}

static void dpn_ReadConn(dpn_conn* c)
{
    for (;;)
    {
        if (c->cap - c->len < 4096)
        {
            size_t ncap = c->cap ? c->cap * 2 : 8192;
            uint8_t* nb = (uint8_t*)realloc(c->buf, ncap);
            if (!nb) { dpn_CloseConn(c); return; }
            c->buf = nb;
            c->cap = ncap;
        }
        long n = recv(c->fd, (char*)c->buf + c->len, (int)(c->cap - c->len), 0);
        if (n > 0) { c->len += (size_t)n; continue; }
        if (n == 0 || !dpn_WouldBlock())
        {
            dpn_Log("connection from %s closed\n", dpn_IpStr(c->ip));
            dpn_DrainConn(c);
            if (c->fd > 0) dpn_CloseConn(c);
            return;
        }
        break;
    }
    dpn_DrainConn(c);
}

static void dpn_ReadUdpSock(int sock)
{
    for (;;)
    {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        long n = recvfrom(sock, (char*)N.rxbuf, sizeof(N.rxbuf), 0, (struct sockaddr*)&from, &flen);
        if (n <= 0) return;

        // System messages can also arrive by UDP; they carry the "play" header.
        if (n >= DPN_HDR && !memcmp(N.rxbuf + 20, "play", 4) && (rd32(N.rxbuf) >> 20) == DPN_TOKEN)
        {
            dpn_HandleSystem(N.rxbuf, (uint32_t)n, from.sin_addr.s_addr);
            continue;
        }
        dpn_OnGameData(N.rxbuf, (uint32_t)n, from.sin_addr.s_addr);
    }
}

// Player message: [DPID from][DPID to][payload]  (session has no NOMESSAGEID)
static void dpn_OnGameData(const uint8_t* data, uint32_t n, uint32_t srcIp)
{
    if (!N.inSession || n < 8) return;
    uint32_t idFrom = rd32(data), idTo = rd32(data + 4);
    dpn_player* to = idTo ? dpn_FindPlayer(idTo) : NULL;
    if (idTo && (!to || !(to->flags & DPF_LOCAL))) return;   // 0 == to all players
    if (N.isHost) dpn_NoteRx(srcIp);
    if (srcIp == N.hostIp) N.lastHostRxMs = dpn_NowMs();
    dpn_PushEvent(DPS_RECV_DATA, idFrom, data + 8, n - 8, NULL);
}

static void dpn_ReadUdp(void) { dpn_ReadUdpSock(N.udp); }

static void dpn_Keepalive(void)
{
    uint64_t now = dpn_NowMs();
    if (N.ipx) dpn_IpxTick();
    if (N.isHost) { dpn_HostTick(); return; }
    if (!N.inSession || !(N.desc.flags & DPS_SESSION_KEEPALIVE)) return;
    if (now - N.lastHostRxMs < DPN_PING_PERIOD_MS || now - N.lastPingMs < DPN_PING_PERIOD_MS) return;

    if (N.missedPings >= DPN_PING_MAX_MISSED)
    {
        dpn_Warn("host stopped answering, session lost\n");
        N.missedPings = 0;
        dpn_PushSys(DPS_SYS_SESSIONLOST, 0x0031, 0);
        return;
    }
    {
        uint8_t m[DPN_HDR + 8];
        dpn_WriteHeader(m, sizeof(m), CMD_PING);
        wr32(m + DPN_HDR, N.localSysId);
        wr32(m + DPN_HDR + 4, (uint32_t)now);
        dpn_SendTcp(N.hostIp, N.hostPort, m, sizeof(m));
    }
    N.lastPingMs = now;
    N.missedPings++;
}

// Services every socket once, optionally waiting up to waitMs for activity.
static void dpn_Pump(int waitMs)
{
    dpn_pfd pfds[DPN_MAX_CONNS + 4];
    int map[DPN_MAX_CONNS + 4];
    int n = 0;

    if (!N.started) return;
    pfds[n].fd = N.tcpListen; pfds[n].events = DPN_PIN; map[n++] = -1;
    pfds[n].fd = N.udp; pfds[n].events = DPN_PIN; map[n++] = -2;
    if (N.isHost && N.hostEnumSock > 0) { pfds[n].fd = N.hostEnumSock; pfds[n].events = DPN_PIN; map[n++] = -3; }
    if (N.ipx && N.ipxSock > 0) { pfds[n].fd = N.ipxSock; pfds[n].events = DPN_PIN; map[n++] = -4; }
    for (int i = 0; i < DPN_MAX_CONNS; i++)
        if (N.conns[i].fd > 0)
        {
            pfds[n].fd = N.conns[i].fd;
            pfds[n].events = (N.conns[i].connecting ? 0 : DPN_PIN) |
                             ((N.conns[i].connecting || N.conns[i].txLen) ? DPN_POUT : 0);
            map[n++] = i;
        }
    dpn_CheckConnectTimeouts();

    if (dpn_Poll(pfds, n, waitMs) <= 0)
    {
        dpn_Keepalive();
        return;
    }

    for (int k = 0; k < n; k++)
    {
        if (!pfds[k].revents) continue;
        if (map[k] == -1)
        {
            for (;;)
            {
                struct sockaddr_in sa;
                socklen_t sl = sizeof(sa);
                int fd = (int)accept(N.tcpListen, (struct sockaddr*)&sa, &sl);
                if (fd < 0) break;
                dpn_NoSigPipe(fd);
                dpn_SetNonBlock(fd);
                dpn_AddConn(fd, 0, sa.sin_addr.s_addr, ntohs(sa.sin_port));
                dpn_Log("accepted %s:%u\n", dpn_IpStr(sa.sin_addr.s_addr), ntohs(sa.sin_port));
            }
        }
        else if (map[k] == -2)
            dpn_ReadUdp();
        else if (map[k] == -3)
            dpn_ReadUdpSock(N.hostEnumSock);
        else if (map[k] == -4)
            dpn_ReadIpx();
        else if (N.conns[map[k]].fd > 0)
        {
            dpn_conn* c = &N.conns[map[k]];
            if (c->connecting || (pfds[k].revents & DPN_POUT))
            {
                dpn_ServiceOutbound(c, pfds[k].revents);
                if (c->fd <= 0 || c->connecting) continue;
            }
            if (pfds[k].revents & (DPN_PIN | DPN_PERR))
                dpn_ReadConn(c);
        }
    }
    dpn_Keepalive();
}

// Pumps until a reply for N.waitCmd arrives or the timeout expires.
static long dpn_WaitReply(int cmd, int timeoutMs)
{
    uint64_t start = dpn_NowMs();
    N.waitCmd = cmd;
    N.gotReply = 0;
    N.replyResult = DPERR_TIMEOUT;
    while (!N.gotReply && dpn_NowMs() - start < (uint64_t)timeoutMs)
    {
        dpn_Pump(50);
        if (!N.isHost && N.hostIp && dpn_IsBadAddr(N.hostIp, N.hostPort))
        {
            N.waitCmd = 0;
            return DPERR_NOCONNECTION;      // couldn't reach the host at all
        }
    }
    N.waitCmd = 0;
    return N.gotReply ? N.replyResult : DPERR_TIMEOUT;
}

// ---------------------------------------------------------------------------
// Sockets setup
// ---------------------------------------------------------------------------

static int dpn_BindRange(int type, uint16_t first, uint16_t last, uint16_t* portOut)
{
    for (uint32_t port = first; port <= last; port++)
    {
        struct sockaddr_in sa;
        int fd = (int)socket(AF_INET, type, 0);
        int one = 1;
        if (fd < 0) return -1;
#ifdef _WIN32
        // Windows' SO_REUSEADDR would let another program take the same port;
        // ask for the port to ourselves instead.
        setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof(one));
        if (type == SOCK_DGRAM) dpn_NoUdpConnReset(fd);
#else
        // TCP only: on UDP, SO_REUSEADDR lets two programs share a port on Linux
        // and silently steal each other's datagrams.
        if (type == SOCK_STREAM) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons((uint16_t)port);
        if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0)
        {
            if (type == SOCK_STREAM && listen(fd, 16) < 0) { dpn_CloseSock(fd); continue; }
            dpn_SetNonBlock(fd);
            *portOut = (uint16_t)port;
            return fd;
        }
        dpn_CloseSock(fd);
    }
    return -1;
}

long dps_Startup(const uint8_t appGuid[16])
{
    const char* lg = getenv("OPENJKDF2_DPLAY_LOG");
    if (N.started) return DP_OK;

#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return DPERR_UNAVAILABLE;
    }
#endif
#if defined(__APPLE__)
    signal(SIGPIPE, SIG_IGN);   // backstop for SO_NOSIGPIPE; the engine doesn't use SIGPIPE
#endif
    memset(&N, 0, sizeof(N));
    memcpy(N.appGuid, appGuid, 16);
    N.log = lg && *lg && *lg != '0';
    N.startMs = dpn_NowMs();

    N.tcpListen = dpn_BindRange(SOCK_STREAM, DPN_TCP_PORT_FIRST, DPN_TCP_PORT_LAST, &N.tcpPort);
    N.udp = dpn_BindRange(SOCK_DGRAM, DPN_UDP_PORT_FIRST, DPN_UDP_PORT_LAST, &N.udpPort);
    N.enumSock = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (N.tcpListen < 0 || N.udp < 0 || N.enumSock < 0)
    {
        dpn_Warn("could not bind DirectPlay ports (TCP %u-%u, UDP %u-%u)\n",
                 DPN_TCP_PORT_FIRST, DPN_TCP_PORT_LAST, DPN_UDP_PORT_FIRST, DPN_UDP_PORT_LAST);
        if (N.tcpListen >= 0) dpn_CloseSock(N.tcpListen);
        if (N.udp >= 0) dpn_CloseSock(N.udp);
        if (N.enumSock >= 0) dpn_CloseSock(N.enumSock);
        memset(&N, 0, sizeof(N));
        return DPERR_UNAVAILABLE;
    }
    { int one = 1; setsockopt(N.enumSock, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof(one)); }
#ifdef _WIN32
    dpn_NoUdpConnReset(N.enumSock);
#endif
    N.started = 1;
    dpn_Log("listening on TCP %u, UDP %u\n", N.tcpPort, N.udpPort);
    return DP_OK;
}

static void dpn_LeaveSession(void);
static void dpn_IpxClose(void);

void dps_Shutdown(void)
{
    if (!N.started) return;
    dpn_LeaveSession();
    dpn_CloseAllConns();
    dpn_CloseSock(N.tcpListen);
    dpn_CloseSock(N.udp);
    dpn_CloseSock(N.enumSock);
    if (N.ipxSock > 0) dpn_CloseSock(N.ipxSock);
    for (int i = 0; i < DPN_IPX_MAX_REASM; i++) free(N.ipxReasm[i].buf);
#ifdef _WIN32
    WSACleanup();
#endif
    dpn_FreeEvents();
    memset(&N, 0, sizeof(N));
}

long dps_Recreate(void)
{
    // Keep sockets; just forget the session and any enumeration state.
    // The IPX socket goes, though: UDP 54792 is only ours while it's in use.
    dpn_LeaveSession();
    N.enumActive = 0;
    memset(N.enumed, 0, sizeof(N.enumed));
    dpn_IpxClose();
    return N.started ? DP_OK : DPERR_UNINITIALIZED;
}

// ---------------------------------------------------------------------------
// Connections (two: TCP/IP and IPX)
// ---------------------------------------------------------------------------

static void dpn_IpxClose(void)
{
    if (N.ipxSock > 0) dpn_CloseSock(N.ipxSock);
    N.ipxSock = 0;
    N.ipx = 0;
    for (int i = 0; i < DPN_IPX_MAX_REASM; i++) free(N.ipxReasm[i].buf);
    memset(N.ipxPeers, 0, sizeof(N.ipxPeers));
    memset(N.ipxPend, 0, sizeof(N.ipxPend));
    memset(N.ipxReasm, 0, sizeof(N.ipxReasm));
    memset(N.ipxDone, 0, sizeof(N.ipxDone));
}

// Takes UDP 54792 and picks our IPX address: network 1 and a random node,
// as IPXWrapper does when it isn't told otherwise.
static long dpn_IpxOpen(void)
{
    int one = 1;
    int fd = dpn_BindRange(SOCK_DGRAM, DPN_IPX_UDP_PORT, DPN_IPX_UDP_PORT, &N.ipxUdpPort);
    if (fd < 0)
    {
        dpn_Warn("IPX: UDP port %u is in use, usually by IPXWrapper in a game running on this machine\n",
                 DPN_IPX_UDP_PORT);
        return DPERR_UNAVAILABLE;
    }
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof(one));
    N.ipxSock = fd;
    memset(N.ipxAddr, 0, sizeof(N.ipxAddr));
    N.ipxAddr[3] = 1;
    wr32(N.ipxAddr + 4, dpn_Random32());
    wr16(N.ipxAddr + 8, (uint16_t)dpn_Random32());
    N.ipxAddr[4] = 0x02;            // locally administered, like IPXWrapper's random nodes
    N.ipxAddr[10] = (uint8_t)(DPN_IPX_SOCKET >> 8);
    N.ipxAddr[11] = (uint8_t)DPN_IPX_SOCKET;
    dpn_Log("IPX: listening on UDP %u, address 00000001.%02x%02x%02x%02x%02x%02x:%u\n", N.ipxUdpPort,
            N.ipxAddr[4], N.ipxAddr[5], N.ipxAddr[6], N.ipxAddr[7], N.ipxAddr[8], N.ipxAddr[9], DPN_IPX_SOCKET);
    return DP_OK;
}

int dpn_IsIpxActive(void) { return N.ipx; }

// Menus are up and the game isn't receiving (the 30 s stats screen between
// levels). Over IPX nothing else acknowledges DirectPlay's guaranteed messages
// or answers its pings, and a JK.EXE peer gives up on us, leaving the next
// level on "Contacting host computer..."; keep the IPX side serviced. (Over
// TCP/IP the kernel acknowledges for us.) Messages queue as usual.
void dpn_Idle(void)
{
    if (N.started && N.ipx && N.inSession) dpn_Pump(0);
}

long dps_EnumConnections(dps_connection_cb cb, void* ctx)
{
    // DPSPGUID_TCPIP {36E95EE0-8577-11CF-960C-0080C7534E82}
    static const uint8_t tcpip[16] = { 0xE0, 0x5E, 0xE9, 0x36, 0x77, 0x85, 0xCF, 0x11,
                                       0x96, 0x0C, 0x00, 0x80, 0xC7, 0x53, 0x4E, 0x82 };
    // "DirectPlay TCP/IP (with JK.EXE)": plays with the original game.
    static const uint16_t name[] = { 'D','i','r','e','c','t','P','l','a','y',' ','T','C','P','/','I','P',' ',
                                     '(','w','i','t','h',' ','J','K','.','E','X','E',')',0 };
    static const uint8_t blob[4] = { 'D', 'P', 'N', '1' };
    // DPSPGUID_IPX {685BC400-9D2C-11CF-A9CD-00AA006886E3}
    static const uint8_t ipx[16] = { 0x00, 0xC4, 0x5B, 0x68, 0x2C, 0x9D, 0xCF, 0x11,
                                     0xA9, 0xCD, 0x00, 0xAA, 0x00, 0x68, 0x86, 0xE3 };
    // "DirectPlay IPX (with JK.EXE + IPXWrapper)"
    static const uint16_t ipxName[] = { 'D','i','r','e','c','t','P','l','a','y',' ','I','P','X',' ',
                                        '(','w','i','t','h',' ','J','K','.','E','X','E',' ','+',' ',
                                        'I','P','X','W','r','a','p','p','e','r',')',0 };
    static const uint8_t ipxBlob[4] = { 'D', 'P', 'N', 'X' };
    if (!N.started) return DPERR_UNINITIALIZED;
    if (cb(ctx, tcpip, name, blob, sizeof(blob)))
        cb(ctx, ipx, ipxName, ipxBlob, sizeof(ipxBlob));
    return DP_OK;
}

long dps_InitializeConnection(const void* connection)
{
    int wantIpx = connection && !memcmp(connection, "DPNX", 4);
    if (!N.started) return DPERR_UNINITIALIZED;
    if (wantIpx && N.ipxSock <= 0)
    {
        long hr = dpn_IpxOpen();
        if (hr != DP_OK) return hr;
    }
    else if (!wantIpx)
        dpn_IpxClose();
    N.ipx = wantIpx;
    return DP_OK;
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

static void dpn_SendEnumTo(uint32_t ipNet)
{
    uint8_t m[DPN_HDR + 24];
    struct sockaddr_in sa;
    dpn_WriteHeader(m, sizeof(m), CMD_ENUMSESSIONS);
    memcpy(m + DPN_HDR, N.appGuid, 16);
    wr32(m + DPN_HDR + 16, 0);      // PasswordOffset
    wr32(m + DPN_HDR + 20, 0x52);   // flags, as sent by JK.EXE (ALL | ASYNC | PASSWORDREQUIRED)
    if (N.ipx)
    {
        // [WIRE] To every node on our network, socket 42367, through UDP 54792.
        uint8_t dst[12];
        memcpy(dst, N.ipxAddr, 4);
        memset(dst + 4, 0xFF, 6);
        dst[10] = (uint8_t)(DPN_IPX_DISC_SOCKET >> 8);
        dst[11] = (uint8_t)DPN_IPX_DISC_SOCKET;
        dpn_IpxSendRaw(dst, ipNet, DPN_IPX_UDP_PORT, m + DPN_SPHDR, sizeof(m) - DPN_SPHDR);
        return;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ipNet;
    sa.sin_port = htons(DPN_ENUM_PORT);
    sendto(N.enumSock, (const char*)m, sizeof(m), 0, (struct sockaddr*)&sa, sizeof(sa));
}

static void dpn_SendEnumRequests(void)
{
    const char* hosts = getenv("OPENJKDF2_DPLAY_HOSTS");

    // IPX searches only go to subnet broadcasts, as IPXWrapper's do.
    if (!N.ipx) dpn_SendEnumTo(htonl(INADDR_BROADCAST));
    if (N.extraEnumHost) dpn_SendEnumTo(N.extraEnumHost);

    // 255.255.255.255 often leaves through one interface only, so also hit
    // every interface's own broadcast address (e.g. a VPN like Radmin's).
    {
        uint32_t bc[16];
        int n = dpn_Broadcasts(bc, 16);
        for (int i = 0; i < n; i++) dpn_SendEnumTo(bc[i]);
    }

    if (hosts && *hosts)
    {
        char buf[512];
        strncpy(buf, hosts, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        for (char* tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", "))
        {
            uint32_t a = inet_addr(tok);
            if (a != INADDR_NONE && a != 0)
                dpn_SendEnumTo(a);
        }
    }
    N.lastEnumSendMs = dpn_NowMs();
}

long dps_EnumSessions(dps_session_cb cb, void* ctx)
{
    uint64_t now;
    if (!N.started) return DPERR_UNINITIALIZED;

    N.enumActive = 1;
    now = dpn_NowMs();
    if (now - N.lastEnumSendMs >= DPN_ENUM_RESEND_MS)
    {
        dpn_SendEnumRequests();
        // Give replies a moment; hosts answer over TCP within a few ms on a LAN/VPN.
        for (int i = 0; i < 6; i++) dpn_Pump(50);
    }
    else
        dpn_Pump(0);

    now = dpn_NowMs();
    for (int i = 0; i < DPN_MAX_ENUMED; i++)
    {
        dpn_enumed* e = &N.enumed[i];
        if (!e->lastSeenMs) continue;
        if (now - e->lastSeenMs > DPN_ENUM_EXPIRE_MS) { memset(e, 0, sizeof(*e)); continue; }
        if (!cb(ctx, &e->desc)) break;
    }
    return DP_OK;
}

void dps_SetEnumHost(const char* host)
{
    struct addrinfo hints, *res = NULL;
    N.extraEnumHost = 0;
    if (!host || !*host) return;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) == 0 && res)
    {
        N.extraEnumHost = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);
    }
}

long dps_StopEnumSessions(void)
{
    N.enumActive = 0;
    return DP_OK;
}

// Session searches arrive on UDP 47624; only one host per machine.
static int dpn_OpenHostEnumSock(void)
{
    struct sockaddr_in sa;
    int fd, one = 1;

    fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof(one));
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof(one));
    dpn_NoUdpConnReset(fd);
#endif
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(DPN_ENUM_PORT);
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0)
    {
#ifdef _WIN32
        dpn_Warn("cannot host: UDP port %u is in use, usually by DirectPlay's helper (dplaysvr.exe) left "
                 "over from running a DirectPlay game: end it in Task Manager\n", DPN_ENUM_PORT);
#else
        dpn_Warn("cannot host: UDP port %u is in use. On Linux this is usually Wine's DirectPlay helper "
                 "(dplaysvr.exe) left over from running a DirectPlay game: pkill -f dplaysvr.exe\n", DPN_ENUM_PORT);
#endif
        dpn_CloseSock(fd);
        return -1;
    }
    dpn_SetNonBlock(fd);
    return fd;
}

long dps_OpenCreate(const dps_session* desc)
{
    int fd = 0;
    uint32_t sysId;

    if (!N.started) return DPERR_UNINITIALIZED;
    if (N.inSession) return DPERR_ACCESSDENIED;

    // Over IPX, searches come to the IPX socket, addressed to socket 42367.
    if (!N.ipx && (fd = dpn_OpenHostEnumSock()) < 0) return DPERR_UNAVAILABLE;

    memset(N.players, 0, sizeof(N.players));
    memset(N.slotUsed, 0, sizeof(N.slotUsed));
    memset(N.slotUniq, 0, sizeof(N.slotUniq));
    N.hostEnumSock = fd;
    N.isHost = 1;
    N.key = dpn_Random32();
    N.desc = *desc;
    for (int i = 0; i < 16; i += 4) wr32(N.desc.guidInstance + i, dpn_Random32());
    N.desc.guidInstance[7] = (uint8_t)((N.desc.guidInstance[7] & 0x0F) | 0x40);   // RFC 4122 v4
    N.desc.guidInstance[8] = (uint8_t)((N.desc.guidInstance[8] & 0x3F) | 0x80);

    // Our system player: the name server.
    sysId = dpn_HostAllocId();
    {
        dpn_player* me = dpn_AddPlayer(sysId);
        me->flags = DPF_SYSTEMPLAYER | DPF_NAMESERVER | DPF_LOCAL;
        me->sysId = sysId;
        me->tcpPort = N.tcpPort;
        me->udpPort = N.udpPort;
    }
    N.localSysId = sysId;
    N.hostIp = 0;
    N.inSession = 1;
    N.enumActive = 0;
    if (N.ipx)
        dpn_Log("hosting over IPX: UDP %u, key %08x\n", N.ipxUdpPort, N.key);
    else
        dpn_Log("hosting: TCP %u, UDP %u, searches on UDP %u, key %08x\n", N.tcpPort, N.udpPort, DPN_ENUM_PORT, N.key);
    return DP_OK;
}

static long dpn_RequestPlayerId(uint32_t flags, uint32_t* idOut)
{
    uint8_t m[DPN_HDR + 4];
    long hr;
    dpn_WriteHeader(m, sizeof(m), CMD_REQUESTPLAYERID);
    wr32(m + DPN_HDR, flags);
    if (dpn_SendTcp(N.hostIp, N.hostPort, m, sizeof(m)) < 0) return DPERR_NOCONNECTION;
    hr = dpn_WaitReply(CMD_REQUESTPLAYERREPLY, DPN_JOIN_TIMEOUT_MS);
    if (hr == DP_OK) *idOut = N.replyId;
    return hr;
}

// ADDFORWARDREQUEST: IDTo, PlayerID, GroupID, CreateOffset, PasswordOffset,
// packed system player, password (UTF-16, NUL-terminated), then the session's
// dwReserved1 echoed back (seen in every JK.EXE capture). m must hold 512 bytes.
static uint32_t dpn_BuildAddForwardRequest(uint8_t* m, uint32_t sysId, const uint16_t* password, uint32_t reserved1)
{
    uint32_t pwBytes = (uint32_t)(dpn_StrLen16(password) + 1) * 2;
    uint32_t packedSize, size;
    uint8_t* pw;

    if (pwBytes > 128) pwBytes = 128;
    packedSize = dpn_WritePackedPlayer(m + DPN_HDR + 20, DPF_SYSTEMPLAYER | DPF_LOCAL, sysId, sysId, NULL);
    size = DPN_HDR + 20 + packedSize + pwBytes + 4;
    dpn_WriteHeader(m, size, CMD_ADDFORWARDREQUEST);
    wr32(m + DPN_HDR + 0, 0);
    wr32(m + DPN_HDR + 4, sysId);
    wr32(m + DPN_HDR + 8, 0);
    wr32(m + DPN_HDR + 12, 0x1C);                   // CreateOffset (from "play")
    wr32(m + DPN_HDR + 16, 0x1C + packedSize);      // PasswordOffset
    pw = m + DPN_HDR + 20 + packedSize;
    memset(pw, 0, pwBytes);
    for (uint32_t i = 0; password && i + 1 < pwBytes / 2 && password[i]; i++)
        wr16(pw + i * 2, password[i]);
    wr32(pw + pwBytes, reserved1);
    return size;
}

long dps_OpenJoin(const uint8_t guidInstance[16], const uint16_t* password)
{
    dpn_enumed* e = NULL;
    uint8_t m[512];
    uint32_t size;
    long hr;

    if (!N.started) return DPERR_UNINITIALIZED;
    if (N.inSession) return DPERR_ACCESSDENIED;

    for (int i = 0; i < DPN_MAX_ENUMED; i++)
        if (N.enumed[i].lastSeenMs && !memcmp(N.enumed[i].desc.guidInstance, guidInstance, 16))
            e = &N.enumed[i];
    if (!e) return DPERR_NOSESSIONS;

    memset(N.players, 0, sizeof(N.players));
    N.hostIp = e->hostIp;
    N.hostPort = e->hostPort;
    N.desc = e->desc;
    dpn_Log("joining %s:%u\n", dpn_IpStr(N.hostIp), N.hostPort);

    // 1. System player ID (flags SYSTEMPLAYER | LOCAL)
    hr = dpn_RequestPlayerId(DPF_SYSTEMPLAYER | DPF_LOCAL, &N.localSysId);
    if (hr != DP_OK) { dpn_Warn("join: no system player id (0x%08lx)\n", (unsigned long)hr); goto fail; }
    {
        dpn_player* me = dpn_AddPlayer(N.localSysId);
        if (me) { me->flags = DPF_SYSTEMPLAYER | DPF_LOCAL; me->sysId = N.localSysId; }
    }

    // 2. ADDFORWARDREQUEST with our system player, password and dwReserved1 echo.
    size = dpn_BuildAddForwardRequest(m, N.localSysId, password, e->reserved1);
    if (dpn_SendTcp(N.hostIp, N.hostPort, m, size) < 0) { hr = DPERR_NOCONNECTION; goto fail; }

    // 3. Wait for the full session state.
    hr = dpn_WaitReply(CMD_SUPERENUMPLAYERSREPLY, DPN_JOIN_TIMEOUT_MS);
    if (hr != DP_OK) { dpn_Warn("join refused or timed out (0x%08lx)\n", (unsigned long)hr); goto fail; }

    {
        dpn_player* me = dpn_FindPlayer(N.localSysId);
        if (me) me->flags |= DPF_LOCAL;
        // The host's own entries carried a zero address: they mean the host.
        // Reach the host's system player on the port we joined through.
        for (int i = 0; i < DPN_MAX_PLAYERS; i++)
        {
            dpn_player* p = &N.players[i];
            if (!p->used) continue;
            if (!p->ip) p->ip = N.hostIp;
            if ((p->flags & DPF_SYSTEMPLAYER) && (p->flags & DPF_NAMESERVER))
            {
                p->ip = N.hostIp;
                p->tcpPort = N.hostPort;
            }
        }
    }

    N.inSession = 1;
    N.enumActive = 0;
    N.lastHostRxMs = dpn_NowMs();
    N.missedPings = 0;
    return DP_OK;

fail:
    dpn_CloseAllConns();
    memset(N.players, 0, sizeof(N.players));
    N.localSysId = 0;
    return hr;
}

// Sends a system message to every other machine (each remote system player).
static void dpn_SendToAllPeers(const uint8_t* m, uint32_t size)
{
    for (int i = 0; i < DPN_MAX_PLAYERS; i++)
    {
        dpn_player* p = &N.players[i];
        if (!p->used || !(p->flags & DPF_SYSTEMPLAYER) || (p->flags & DPF_LOCAL)) continue;
        if (!p->ip || !p->tcpPort) continue;
        if (p->unreachableUntilMs && dpn_NowMs() < p->unreachableUntilMs) continue;
        if (dpn_SendTcp(p->ip, p->tcpPort, m, size) < 0)
        {
            dpn_Warn("peer %08x (%s) unreachable over TCP; is port %u open on their side?\n",
                     p->id, dpn_IpStr(p->ip), p->tcpPort);
            p->unreachableUntilMs = dpn_NowMs() + 30000;
        }
    }
}

static void dpn_SendDeletePlayer(uint32_t id)
{
    uint8_t m[DPN_HDR + 20];
    dpn_WriteHeader(m, sizeof(m), CMD_DELETEPLAYER);
    memset(m + DPN_HDR, 0, 20);
    wr32(m + DPN_HDR + 4, id);
    dpn_SendToAllPeers(m, sizeof(m));
}

static void dpn_LeaveSession(void)
{
    if (!N.inSession) return;
    // JK.EXE deletes its game player(s) first, then its system player.
    for (int i = 0; i < DPN_MAX_PLAYERS; i++)
        if (N.players[i].used && (N.players[i].flags & DPF_LOCAL) && !(N.players[i].flags & DPF_SYSTEMPLAYER))
            dpn_SendDeletePlayer(N.players[i].id);
    if (N.localSysId)
        dpn_SendDeletePlayer(N.localSysId);
    dpn_CloseAllConns();
    memset(N.players, 0, sizeof(N.players));
    memset(&N.desc, 0, sizeof(N.desc));
    if (N.isHost)
    {
        if (N.hostEnumSock > 0) dpn_CloseSock(N.hostEnumSock);
        N.hostEnumSock = 0;
        N.isHost = 0;
        memset(N.slotUsed, 0, sizeof(N.slotUsed));
    }
    N.inSession = 0;
    N.localSysId = 0;
    N.hostIp = 0;
    dpn_FreeEvents();
}

long dps_Close(void)
{
    dpn_LeaveSession();
    return DP_OK;
}

long dps_GetSessionDesc(dps_session* out)
{
    if (!N.inSession) return DPERR_NOCONNECTION;
    *out = N.desc;
    if (N.isHost) out->curPlayers = dpn_CountGamePlayers();
    return DP_OK;
}

long dps_SetSessionDesc(const dps_session* in)
{
    uint8_t guid[16];
    if (!N.isHost) return DPERR_ACCESSDENIED; // only the host may change it
    memcpy(guid, N.desc.guidInstance, 16);
    N.desc = *in;
    memcpy(N.desc.guidInstance, guid, 16);
    // New searches see the change immediately. [SPEC] Joined machines would
    // also get DPSP_MSG_SESSIONDESCCHANGED; not sent yet (no capture of it).
    return DP_OK;
}

// ---------------------------------------------------------------------------
// Players
// ---------------------------------------------------------------------------

long dps_CreatePlayer(const uint16_t* shortName, uint32_t* idOut)
{
    uint8_t m[DPN_HDR + 20 + DPN_PACKED_FIXED + 64 + 32 + 6];
    uint32_t id = 0, packedSize, size;
    uint16_t name[32];
    dpn_player* me;
    long hr;

    if (!N.inSession) return DPERR_NOCONNECTION;

    if (N.isHost)
    {
        id = dpn_HostAllocId();
        if (!id) return DPERR_CANTCREATEPLAYER;
    }
    else
    {
        hr = dpn_RequestPlayerId(DPF_LOCAL, &id);
        if (hr != DP_OK) return hr;
    }

    memset(name, 0, sizeof(name));
    for (size_t i = 0; shortName && i < 31 && shortName[i]; i++) name[i] = shortName[i];

    me = dpn_AddPlayer(id);
    if (!me) return DPERR_OUTOFMEMORY;
    me->flags = DPF_LOCAL;
    me->sysId = N.localSysId;
    memcpy(me->name, name, sizeof(name));

    // CREATEPLAYER: IDTo, PlayerID, GroupID, CreateOffset, PasswordOffset,
    // packed player, then 6 trailing zero bytes (as JK.EXE sends).
    packedSize = dpn_WritePackedPlayer(m + DPN_HDR + 20, DPF_LOCAL, id, N.localSysId, name);
    size = DPN_HDR + 20 + packedSize + 6;
    dpn_WriteHeader(m, size, CMD_CREATEPLAYER);
    wr32(m + DPN_HDR + 0, 0);
    wr32(m + DPN_HDR + 4, id);
    wr32(m + DPN_HDR + 8, 0);
    wr32(m + DPN_HDR + 12, 0x1C);
    wr32(m + DPN_HDR + 16, 0);
    memset(m + DPN_HDR + 20 + packedSize, 0, 6);
    dpn_SendToAllPeers(m, size);

    if (idOut) *idOut = id;
    return DP_OK;
}

long dps_DestroyPlayer(uint32_t id)
{
    dpn_player* p = dpn_FindPlayer(id);
    if (!p || !(p->flags & DPF_LOCAL)) return DPERR_INVALIDPLAYER;
    dpn_SendDeletePlayer(id);
    p->used = 0;
    return DP_OK;
}

long dps_EnumPlayers(dps_player_cb cb, void* ctx)
{
    if (!N.inSession) return DPERR_NOCONNECTION;
    for (int i = 0; i < DPN_MAX_PLAYERS; i++)
    {
        dpn_player* p = &N.players[i];
        if (!p->used || (p->flags & DPF_SYSTEMPLAYER)) continue;
        if (!cb(ctx, p->id, p->name[0] ? p->name : NULL, NULL)) break;
    }
    return DP_OK;
}

// ---------------------------------------------------------------------------
// Messaging
// ---------------------------------------------------------------------------

static long dpn_SendUdpTo(dpn_player* to, const uint8_t* buf, uint32_t len)
{
    struct sockaddr_in sa;
    if (!to || !to->ip || !to->udpPort || (to->flags & DPF_LOCAL)) return DPERR_INVALIDPLAYER;
    if (N.ipx) return dpn_IpxSendToPeer(to->ip, buf, len) < 0 ? DPERR_GENERIC : DP_OK;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = to->ip;
    sa.sin_port = htons(to->udpPort);
    return sendto(N.udp, (const char*)buf, (int)len, 0, (struct sockaddr*)&sa, sizeof(sa)) < 0 ? DPERR_GENERIC : DP_OK;
}

long dps_Send(uint32_t idFrom, uint32_t idTo, const void* data, uint32_t size)
{
    uint8_t buf[4096];

    if (!N.inSession) return DPERR_NOCONNECTION;
    if (size + 8 > sizeof(buf)) return DPERR_GENERIC;

    wr32(buf, idFrom);
    wr32(buf + 4, idTo);
    memcpy(buf + 8, data, size);

    // [WIRE] idTo == 0 is DPID_ALLPLAYERS: JK sends DSS_JOINREQUEST that way
    // (its server id isn't known yet), and JK.EXE delivers one copy, still
    // addressed to 0, to every other machine in the session.
    if (idTo == 0)
    {
        for (int i = 0; i < DPN_MAX_PLAYERS; i++)
        {
            dpn_player* p = &N.players[i];
            if (p->used && (p->flags & DPF_SYSTEMPLAYER) && !(p->flags & DPF_LOCAL))
                dpn_SendUdpTo(p, buf, size + 8);
        }
        return DP_OK;
    }

    return dpn_SendUdpTo(dpn_AddrOf(dpn_FindPlayer(idTo)), buf, size + 8);
}

int dps_Receive(uint32_t* fromOut, const void** dataOut, uint32_t* sizeOut, dps_sysmsg* sysOut)
{
    static dpn_event* last = NULL; // returned data stays valid until the next call
    dpn_event* e;

    free(last);
    last = NULL;

    if (!N.started) return DPS_RECV_NONE;
    if (!N.evHead) dpn_Pump(0);
    e = N.evHead;
    if (!e) return DPS_RECV_NONE;
    N.evHead = e->next;
    if (!N.evHead) N.evTail = NULL;
    last = e;

    if (e->recvType == DPS_RECV_DATA)
    {
        *fromOut = e->from;
        *dataOut = e->data;
        *sizeOut = e->size;
        return DPS_RECV_DATA;
    }
    *sysOut = e->sys;
    return DPS_RECV_SYS;
}

// ---------------------------------------------------------------------------
// Test hook: feed a raw captured system message through the parser.
// ---------------------------------------------------------------------------
#ifdef DPLAY_NATIVE_TESTS
void dpn_TestFeed(const uint8_t* msg, uint32_t size, uint32_t srcIpNet, int asWaitCmd)
{
    N.waitCmd = asWaitCmd;
    dpn_HandleSystem(msg, size, srcIpNet);
    N.waitCmd = 0;
}
void dpn_TestBegin(const uint8_t appGuid[16]) { memset(&N, 0, sizeof(N)); memcpy(N.appGuid, appGuid, 16); N.log = 1; N.enumActive = 1; N.tcpPort = 2300; N.udpPort = 2350; }
int dpn_TestPlayers(void (*fn)(uint32_t id, uint32_t flags, uint32_t sys, const uint16_t* name, uint32_t ip, uint16_t tcp, uint16_t udp))
{
    int n = 0;
    for (int i = 0; i < DPN_MAX_PLAYERS; i++)
        if (N.players[i].used) { fn(N.players[i].id, N.players[i].flags, N.players[i].sysId, N.players[i].name, N.players[i].ip, N.players[i].tcpPort, N.players[i].udpPort); n++; }
    return n;
}
const dps_session* dpn_TestDesc(void) { return &N.desc; }
const dps_session* dpn_TestEnumed(int i) { return N.enumed[i].lastSeenMs ? &N.enumed[i].desc : NULL; }
// Host-side builders, captured instead of sent. Recreates the vanilla host's
// state from the capture: key, instance GUID, session fields.
int dpn_TestHostEnumReply(uint8_t* out, const uint8_t guid[16], uint32_t key, const dps_session* d, uint32_t curPlayers)
{
    N.isHost = 1; N.key = key; N.desc = *d; memcpy(N.desc.guidInstance, guid, 16);
    memset(N.players, 0, sizeof(N.players));
    for (uint32_t i = 0; i < curPlayers; i++) { dpn_player* p = dpn_AddPlayer(0x1000 + i); p->flags = 0; }
    N.tcpPort = 2300; dpn_testCapture = 1;
    dpn_HostSendEnumReply(0, 2300);
    dpn_testCapture = 0;
    memcpy(out, dpn_testBuf, dpn_testLen);
    return (int)dpn_testLen;
}
int dpn_TestHostPlayerReply(uint8_t* out, uint32_t id)
{
    dpn_testCapture = 1; dpn_HostSendPlayerReply(0, 2300, id); dpn_testCapture = 0;
    memcpy(out, dpn_testBuf, dpn_testLen);
    return (int)dpn_testLen;
}
int dpn_TestHostAddForward(uint8_t* out, uint32_t id, uint32_t ipNet)
{
    uint8_t* m = out;
    uint32_t packed = dpn_WritePackedPlayerEx(m + DPN_HDR + 20, DPF_SYSTEMPLAYER | DPF_INGROUP, id, id, NULL, ipNet, 2300, 2350);
    uint32_t msize = DPN_HDR + 20 + packed;
    dpn_WriteHeader(m, msize, CMD_ADDFORWARD);
    wr32(m + DPN_HDR + 0, 0); wr32(m + DPN_HDR + 4, id); wr32(m + DPN_HDR + 8, 0);
    wr32(m + DPN_HDR + 12, 0x1C); wr32(m + DPN_HDR + 16, 0);
    return (int)msize;
}

int dpn_TestBuildAddForwardRequest(uint8_t* out, uint32_t sysId, uint32_t reserved1) { return (int)dpn_BuildAddForwardRequest(out, sysId, NULL, reserved1); }
int dpn_TestBuildAddForwardRequestPw(uint8_t* out, uint32_t sysId, const uint16_t* pw, uint32_t reserved1) { return (int)dpn_BuildAddForwardRequest(out, sysId, pw, reserved1); }

// IPX: we are the machine at addr; a datagram arrives from ip:port. Returns
// whether it completed the reply we're waiting for; sent gets what we sent back.
void dpn_TestIpxBegin(const uint8_t appGuid[16], const uint8_t addr[12])
{
    dpn_TestBegin(appGuid);
    N.ipx = 1;
    memcpy(N.ipxAddr, addr, 12);
}
int dpn_TestIpxPacket(const uint8_t* pkt, uint32_t n, uint32_t ipNet, uint16_t port, int asWaitCmd, uint8_t* sent, uint32_t* sentLen)
{
    N.waitCmd = asWaitCmd;
    N.gotReply = 0;
    dpn_testCapture = 1;
    dpn_testLen = 0;
    dpn_IpxOnPacket(pkt, n, ipNet, port);
    dpn_testCapture = 0;
    N.waitCmd = 0;
    if (sent) memcpy(sent, dpn_testBuf, dpn_testLen);
    if (sentLen) *sentLen = dpn_testLen;
    return N.gotReply;
}
uint32_t dpn_TestReplyId(void) { return N.replyId; }
int dpn_TestIpxPeer(uint32_t handle, uint8_t addr[12], uint32_t* ipNet, uint16_t* port)
{
    dpn_ipxpeer* p = dpn_IpxPeerOf(handle);
    if (!p) return 0;
    memcpy(addr, p->addr, 12);
    *ipNet = p->ip;
    *port = p->port;
    return 1;
}
int dpn_TestBuildCreatePlayer(uint8_t* out, uint32_t id, uint32_t sysId, const uint16_t* name)
{
    uint32_t packedSize = dpn_WritePackedPlayer(out + DPN_HDR + 20, DPF_LOCAL, id, sysId, name);
    uint32_t size = DPN_HDR + 20 + packedSize + 6;
    dpn_WriteHeader(out, size, CMD_CREATEPLAYER);
    wr32(out + DPN_HDR + 0, 0); wr32(out + DPN_HDR + 4, id); wr32(out + DPN_HDR + 8, 0);
    wr32(out + DPN_HDR + 12, 0x1C); wr32(out + DPN_HDR + 16, 0);
    memset(out + DPN_HDR + 20 + packedSize, 0, 6);
    return (int)size;
}
#endif

const dps_backend dpnative_backend = DPS_BACKEND_TABLE(dpnative);

#endif // PLATFORM_DPLAY && DPLAY_NATIVE
