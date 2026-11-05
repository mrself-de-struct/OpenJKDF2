// Map and mod transfers on the web lobby, both sides, the jkdf2mp browser
// build's way: packaging/wasm/maps.js and mods.js in
// github.com/tristamus/OpenJKDF2 (web-mp 51b96827). Names, limits and checks
// follow those files so a desktop joiner gets exactly what a browser one does.
//
// Frames travel like game packets, over the "jk-map" WebRTC channel when it's
// open, else through the lobby (jkweb_client.c):
//   u32 idFrom, u32 idTo, "JKMT", u8 type, u8 version 1, u8 kind, u8 0
//   joiner -> host   INFO_REQ (1)
//                    CHUNK_REQ (3)  <id:8> <chunkSize u32> <first u32> <count u32>
//                    BLOCK_REQ (5)  <id:8> <first u32> <count u32> <partSize u32>
//                                   <accept u8> <pad:3>
//   host -> joiner   INFO (2)       <JSON: status, name, ext, size, sha256,
//                                    streamSize, refsBytes, z>
//                    CHUNK (4)      <id:8> <index u32> <data>
//                    BLOCK (6)      <id:8> <block u32> <zlen u32> <at u32>
//                                   <codec u8> <pad:3> <data>
// Blocks (tristamus' maps.js, 2026-10-29): a host that says z (BLOCK_BYTES) in
// its INFO also sends the stream in 1 MiB blocks, raw-deflated when the joiner
// can inflate (accept 1) and it makes the block smaller (codec 1, else 0), in
// parts of partSize: zlen is the block as sent, at where the part goes in it.
// A joiner asks a host that says z for blocks, any other for chunks.
// <id> is the first 8 bytes of the file's SHA-256. The stream the chunks cut
// up is a reference table (u32 count, then per reference u32 offset, u32 size,
// SHA-256 of those bytes), then the file's bytes outside the references. Each
// reference is a copy of a file in the retail GOBs (or of a whole one), which
// the joiner fills in from its own copy; the rebuilt file has to match the
// host's SHA-256.
//
// A joiner puts a map in episode/<name>.<ext> (a different local copy is
// kept as <name>.<ext>.bak) and unpacks a mod's pack (jkmod.c) into
// netmods/<first 16 hex digits of its fingerprint>, where jkmod_FindLocal
// finds it next time. A host offers its episode GOB and its running mod,
// each worked out once when it starts hosting (maps.js host(), mods.js
// host()), with every part that is a copy of a retail file sent as a
// reference (planShareRanges()).

#if defined(PLATFORM_DPLAY) && (defined(PLATFORM_CURL) || defined(JKX_TESTS))

#include "jkweb_xfer.h"
#include "jkmod.h"

#include <ctype.h>
#include <math.h>
#ifndef JKX_NO_ZLIB
#include <zlib.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define JKX_SEP "\\"
#else
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#define JKX_SEP "/"
#endif

#define T_INFO_REQ   1
#define T_INFO       2
#define T_CHUNK_REQ  3
#define T_CHUNK      4
#define T_BLOCK_REQ  5
#define T_BLOCK      6
#define HEADER       16
#define VERSION      1

#define MAX_MAP_BYTES       (1024u * 1024 * 1024)   // Changed: 1 GB, as maps.js since 2026-10-29
#define MAX_MOD_BYTES       (1024u * 1024 * 1024)
#define MAX_MOD_FILES       10000
#define MAX_SEND_BYTES      ((double)MAX_MOD_BYTES + 16 + MAX_MOD_FILES * 136.0)
#define MAX_REFS            65536
#define REF_BYTES           40
#define CHUNK_DIRECT        (16 * 1024)
#define CHUNK_RELAY         (48 * 1024)
#define WINDOW_BYTES        (1024 * 1024)
#define MAX_CHUNKS_PER_REQ  128
#define DIRECT_WAIT_MS      3000
#define INFO_RESEND_MS      5000
#define INFO_TIMEOUT_MS     20000
// Our own: with a copy of the map already here, a host that doesn't answer
// (an older page, or a desktop host, which doesn't serve maps yet) only holds
// the join up this long before ours is used, as the browser does after 20 s.
#define INFO_TIMEOUT_LOCAL_MS 6000
#define CHUNK_RESEND_MS     5000
#define BLOCK_BYTES         (1024 * 1024)
#define PART_DIRECT         (64 * 1024)
#define PART_RELAY          (48 * 1024)
#define BLOCK_HEADER        24
#define MAX_BLOCKS_PER_REQ  4
#define BLOCKS_DIRECT       3       // blocks asked for and not yet in, on a direct way
#define BLOCKS_RELAY        2       // ...through the lobby
#define BLOCK_UNIT          (16 * 1024)   // parts come in whole units of this
#define CODEC_STORED        0
#define CODEC_DEFLATE       1
#define STALL_MS            15000
#define GOB_ENTRY           136

enum { S_IDLE, S_INFO, S_ASK, S_WAIT_DIRECT, S_DOWNLOAD, S_REBUILD };

// Our own: stop at S_ASK for the player's yes/no before downloading (jkx_SetAskFirst)
static int jkx_bAskFirst = 0;

static const char* jkx_retailEpisodes[] = {
    "jk1", "jk1ctf", "jk1mp", "jk1demo", "jk1mpdem", "jk1mpdemo",
    "jkm", "jkm_kfy", "jkm_mp", "jkm_saber", NULL
};

static const char* jkx_retailGob[] = {
    "episode/JK1.gob", "episode/JK1CTF.gob", "episode/JK1MP.gob",
    "episode/jk1demo.gob", "episode/jk1mpdem.gob", "episode/jk1mpdemo.gob",
    "resource/Res1hi.gob", "resource/Res1low.gob", "resource/Res2.gob",
    "resource/res1demo.gob", NULL
};

static const char* jkx_retailGoo[] = {
    "episode/JKM.goo", "episode/JKM_KFY.goo", "episode/JKM_MP.goo", "episode/JKM_SABER.goo",
    "resource/Jkmres.goo", "resource/JKMsndLO.goo", NULL
};

static struct
{
    int      kind, state, result;
    char     name[64], ext[8];
    uint8_t  fp[32];
    char     reason[32], message[320], status[160], resultPath[512], ask[320];
    uint64_t started, lastAsk, phaseStart;
    int      haveLocal;
    char     localPath[512];

    // INFO
    int      gotInfo;
    char     infoStatus[32], infoName[64], infoExt[16], infoSha[72];
    double   size, streamSize, refsBytes;
    uint8_t  sha[32];

    // download
    double   z;              // the INFO's block size: blocks, not chunks, if it's BLOCK_BYTES
    int      blocks;         // downloading in blocks
    uint32_t chunkSize, n, received, next, window, batch, askedCount, total;
    uint8_t* buf;
    uint8_t* got;
    uint64_t* askedAt;
    uint64_t bytes, lastProgress, lastResendCheck;
    int      via;
} X;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

#ifdef JKX_TESTS
uint64_t jkx_TestNowMs(void);
static uint64_t jkx_NowMs(void) { return jkx_TestNowMs(); }
#else
static uint64_t jkx_NowMs(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}
#endif

static void jkx_Log(const char* fmt, ...)
{
    va_list ap;
    fprintf(stderr, "OpenJKDF2: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static int jkx_EqNoCase(const char* a, const char* b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return !*a && !*b;
}

static int jkx_NEqNoCase(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

static int jkx_IsFile(const char* path, uint64_t* sizeOut)
{
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    if (sizeOut) *sizeOut = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return 1;
#else
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    if (sizeOut) *sizeOut = (uint64_t)st.st_size;
    return 1;
#endif
}

static int jkx_IsDir(const char* path)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int jkx_MkDir(const char* path)
{
#ifdef _WIN32
    return _mkdir(path) == 0 || jkx_IsDir(path);
#else
    return mkdir(path, 0755) == 0 || jkx_IsDir(path);
#endif
}

// Every folder in path up to its last separator.
static int jkx_MkParents(const char* path)
{
    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return 0;
    memcpy(tmp, path, len + 1);
    for (size_t i = 1; i < len; i++)
    {
        if (tmp[i] != '/' && tmp[i] != '\\') continue;
        tmp[i] = 0;
        if (!jkx_MkDir(tmp)) return 0;
        tmp[i] = JKX_SEP[0];
    }
    return 1;
}

// "a/b.gob" under the game folder, whatever its case (Linux game folders are
// often copied from Windows). 1 if found; out gets the real path.
static int jkx_FindNoCase(const char* rel, char* out, size_t outLen)
{
#ifdef _WIN32
    snprintf(out, outLen, "%s", rel);
    for (char* p = out; *p; p++) if (*p == '/') *p = '\\';
    return jkx_IsFile(out, NULL) || jkx_IsDir(out);
#else
    char path[1024] = ".";
    const char* p = rel;
    while (*p)
    {
        const char* slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        char part[256];
        DIR* d;
        struct dirent* e;
        int found = 0;
        if (n == 0 || n >= sizeof(part)) return 0;
        memcpy(part, p, n);
        part[n] = 0;
        d = opendir(path);
        if (!d) return 0;
        while ((e = readdir(d)))
        {
            if (jkx_EqNoCase(e->d_name, part))
            {
                size_t l = strlen(path);
                snprintf(path + l, sizeof(path) - l, "/%s", e->d_name);
                found = 1;
                break;
            }
        }
        closedir(d);
        if (!found) return 0;
        p = slash ? slash + 1 : p + n;
    }
    const char* found = path[0] == '.' && path[1] == '/' ? path + 2 : path;
    if (strlen(found) >= outLen) return 0; // a cut-off path would name some other file
    memcpy(out, found, strlen(found) + 1);
    return 1;
#endif
}

static int jkx_ReadRange(const char* path, uint64_t offset, uint8_t* out, uint64_t size)
{
    FILE* f = fopen(path, "rb");
    int ok;
    if (!f) return 0;
#ifdef _WIN32
    ok = _fseeki64(f, (__int64)offset, SEEK_SET) == 0;
#else
    ok = fseeko(f, (off_t)offset, SEEK_SET) == 0;
#endif
    ok = ok && fread(out, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    return ok;
}

static void jkx_Hex(const uint8_t* b, size_t n, char* out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2*i] = d[b[i] >> 4]; out[2*i+1] = d[b[i] & 15]; }
    out[2*n] = 0;
}

static void jkx_Mb(char* out, size_t len, double n)
{
    snprintf(out, len, n < 10485760 ? "%.1f" : "%.0f", n / 1048576.0);
}

// ---------------------------------------------------------------------------
// Names and messages (maps.js / mods.js)
// ---------------------------------------------------------------------------

static int jkx_IsRetailEpisode(const char* name)
{
    for (int i = 0; jkx_retailEpisodes[i]; i++)
        if (jkx_EqNoCase(name, jkx_retailEpisodes[i])) return 1;
    return 0;
}

// validEpisodeName(): it becomes a file name here, so at most 31 characters,
// nothing Windows wouldn't allow, and no way out of episode/.
static int jkx_ValidEpisodeName(const char* name)
{
    size_t len = strlen(name);
    if (!len || len > 31 || name[0] == '.' || strstr(name, "..")) return 0;
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || strchr("/\\:*?\"<>|", c)) return 0;
    }
    return 1;
}

// Our own: a host can keep maps and mods to themselves by listing them in
// noshare.txt in the game folder, one per line ("# ..." comments). A line is a
// file or folder name, with any path in front ignored, matched without regard
// to case against the episode GOB or the mod's folder/GOB, with or without its
// extension; "*" and "?" are wildcards. Such a file is answered "not-shared".
#define JKX_NOSHARE_FILE "noshare.txt"

static int jkx_WildNoCase(const char* pat, const char* s)
{
    if (!*pat) return !*s;
    if (*pat == '*')
    {
        for (;; s++)
        {
            if (jkx_WildNoCase(pat + 1, s)) return 1;
            if (!*s) return 0;
        }
    }
    if (!*s) return 0;
    if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*s)) return 0;
    return jkx_WildNoCase(pat + 1, s + 1);
}

// The line of noshare.txt that holds back this file or folder, or 0.
static int jkx_NoShare(const char* path)
{
    char listPath[512], line[256], base[256];
    const char* b = path;
    char* dot;
    FILE* f;
    int lineNo = 0, hit = 0;

    for (const char* p = path; *p; p++)
        if ((*p == '/' || *p == '\\') && p[1]) b = p + 1;
    snprintf(base, sizeof(base), "%s", b);
    if (base[0] && (base[strlen(base) - 1] == '/' || base[strlen(base) - 1] == '\\')) base[strlen(base) - 1] = 0;
    if (!base[0] || !jkx_FindNoCase(JKX_NOSHARE_FILE, listPath, sizeof(listPath))) return 0;
    f = fopen(listPath, "r");
    if (!f) return 0;
    dot = strrchr(base, '.');
    while (!hit && fgets(line, sizeof(line), f))
    {
        char* s = line;
        char* e;
        const char* pat;
        lineNo++;
        while (*s == ' ' || *s == '\t') s++;
        e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (!*s || *s == '#') continue;
        if (e > s && (e[-1] == '/' || e[-1] == '\\')) *--e = 0;
        pat = s;
        for (const char* p = s; *p; p++)
            if ((*p == '/' || *p == '\\') && p[1]) pat = p + 1;
        if (jkx_WildNoCase(pat, base)) hit = lineNo;
        else if (dot)
        {
            *dot = 0;
            if (jkx_WildNoCase(pat, base)) hit = lineNo;
            *dot = '.';
        }
    }
    fclose(f);
    return hit;
}

static const char* jkx_GameTitle(const char* ext)
{
    return !strcmp(ext, "goo") ? "Mysteries of the Sith" : "Jedi Knight";
}

static void jkx_SetMessage(const char* reason)
{
    const char* what = X.kind == JKX_KIND_MOD ? "mod" : "map";
    const char* m = NULL;
    char buf[320];
    buf[0] = 0;

    if (X.kind == JKX_KIND_MOD)
    {
        if (!strcmp(reason, "none")) m = "The host's mod couldn't be read on their side.";
        else if (!strcmp(reason, "error")) m = "The host couldn't get its mod ready to send.";
        else if (!strcmp(reason, "too-big")) m = "This mod is over 100 MB, too big to send. To join, put your own copy of it in your expansions folder.";
        else if (!strcmp(reason, "too-many-files")) m = "This mod has too many files to send.";
        else if (!strcmp(reason, "retail-only")) m = "The host's mod is made only of original game files, so it isn't sent.";
        else if (!strcmp(reason, "bad-files")) m = "The host's mod has file names that can't be saved here, so it wasn't installed.";
        else if (!strcmp(reason, "changed")) m = "The host's mod files changed after their game started. They need to reload their page.";
        else if (!strcmp(reason, "other-mod")) m = "The host is running a different mod now.";
        else if (!strcmp(reason, "install")) m = "The mod couldn't be saved in the netmods folder.";
        else if (!strcmp(reason, "not-shared")) m = "The host has chosen not to share this mod. To join, get your own copy and put it in your expansions folder.";
    }
    else
    {
        if (!strcmp(reason, "none")) m = "The host's map isn't a single map file on their side, so it can't be sent.";
        else if (!strcmp(reason, "too-big")) m = "This map is over 100 MB, too big to send.";
        else if (!strcmp(reason, "not-gob")) m = "The host's map file isn't a GOB the game can read.";
        else if (!strcmp(reason, "retail-only")) m = "The host's map is made only of original game files, so it isn't sent.";
        else if (!strcmp(reason, "too-many-files")) m = "This map has too many files to send.";
        else if (!strcmp(reason, "install")) m = "The map couldn't be saved in the episode folder.";
        else if (!strcmp(reason, "bad-name")) m = "The host's map name isn't one that can be saved.";
        else if (!strcmp(reason, "retail")) m = "The host is running an original level, which isn't sent.";
        else if (!strcmp(reason, "other-map")) m = "The host is running a different map now.";
        else if (!strcmp(reason, "error")) m = "The host couldn't get its map ready to send.";
        else if (!strcmp(reason, "not-shared")) m = "The host has chosen not to share this map. To join, get your own copy and put it in your episode folder.";
        else if (!strcmp(reason, "retail-missing"))
        {
            snprintf(buf, sizeof(buf), "This game runs an original %s level that isn't in your game folder.",
                     jkx_GameTitle(X.ext));
            m = buf;
        }
    }
    if (!m)
    {
        if (!strcmp(reason, "no-answer")) m = "The host didn't answer. They may need to reload their page.";
        else if (!strcmp(reason, "stalled")) m = "The download stopped. The connection to the host may have dropped.";
        else if (!strcmp(reason, "host-left")) m = "The host left the game.";
        else if (!strcmp(reason, "lobby-lost")) m = "The connection to the lobby dropped.";
        else if (!strcmp(reason, "corrupt") || !strcmp(reason, "mismatch"))
        {
            snprintf(buf, sizeof(buf), "The %s didn't arrive intact, so it wasn't installed.", what);
            m = buf;
        }
        else if (!strcmp(reason, "other-game"))
        {
            snprintf(buf, sizeof(buf), "This game is for %s, and you're running %s.",
                     jkx_GameTitle(!strcmp(X.ext, "goo") ? "gob" : "goo"), jkx_GameTitle(X.ext));
            m = buf;
        }
        else if (!strcmp(reason, "missing-retail"))
        {
            snprintf(buf, sizeof(buf), "This %s uses original %s files that weren't found in your game folder.",
                     what, jkx_GameTitle(X.ext));
            m = buf;
        }
        else if (!strcmp(reason, "cancelled") || !strcmp(reason, "declined")) m = "";
        else
        {
            snprintf(buf, sizeof(buf), "The host's %s couldn't be sent (%s).", what, reason);
            m = buf;
        }
    }
    snprintf(X.message, sizeof(X.message), "%s", m);
}

static void jkx_FreeBlocks(void);

static void jkx_FreeDownload(void)
{
    jkx_FreeBlocks();
    free(X.buf); X.buf = NULL;
    free(X.got); X.got = NULL;
    free(X.askedAt); X.askedAt = NULL;
}

static int jkx_Fail(const char* reason)
{
    if (X.result != JKX_BUSY) return X.result;
    snprintf(X.reason, sizeof(X.reason), "%s", reason);
    jkx_SetMessage(reason);
    X.result = JKX_FAILED;
    X.state = S_IDLE;
    jkx_FreeDownload();
    jkx_Log("%s %s not installed: %s", X.kind == JKX_KIND_MOD ? "mod" : "map", X.name, reason);
    return X.result;
}

static int jkx_Ready(void)
{
    X.result = JKX_READY;
    X.state = S_IDLE;
    jkx_FreeDownload();
    return X.result;
}

// ---------------------------------------------------------------------------
// GOBs and the joiner's retail files
// ---------------------------------------------------------------------------

typedef struct { uint32_t offset, size; char name[129]; } jkx_gobEntry;

// The file list of a GOB (read through a callback), or -1 if it isn't one.
typedef int (*jkx_reader)(void* ctx, uint64_t off, uint8_t* out, uint64_t n);

static int jkx_ParseGob(jkx_reader read, void* ctx, uint64_t fileSize, jkx_gobEntry** outEntries)
{
    uint8_t head[12], cnt[4];
    uint32_t tableOffs, count;
    uint8_t* table;
    jkx_gobEntry* e;
    *outEntries = NULL;
    if (fileSize < 16 || !read(ctx, 0, head, 12)) return -1;
    if (memcmp(head, "GOB ", 4) || rd32(head + 4) != 20) return -1;
    tableOffs = rd32(head + 8);
    if ((uint64_t)tableOffs + 4 > fileSize || !read(ctx, tableOffs, cnt, 4)) return -1;
    count = rd32(cnt);
    if ((uint64_t)tableOffs + 4 + (uint64_t)count * GOB_ENTRY > fileSize) return -1;
    if (!count) return 0;
    table = (uint8_t*)malloc((size_t)count * GOB_ENTRY);
    e = (jkx_gobEntry*)calloc(count, sizeof(jkx_gobEntry));
    if (!table || !e || !read(ctx, (uint64_t)tableOffs + 4, table, (uint64_t)count * GOB_ENTRY)) { free(table); free(e); return -1; }
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t* t = table + (size_t)i * GOB_ENTRY;
        int32_t size = (int32_t)rd32(t + 4);
        int j;
        e[i].offset = rd32(t);
        if (size < 0 || (uint64_t)e[i].offset + (uint32_t)size > fileSize) { free(table); free(e); return -1; }
        e[i].size = (uint32_t)size;
        for (j = 0; j < 128 && t[8 + j]; j++) e[i].name[j] = (char)t[8 + j];
        e[i].name[j] = 0;
    }
    free(table);
    *outEntries = e;
    return (int)count;
}

static int jkx_FileRead(void* ctx, uint64_t off, uint8_t* out, uint64_t n) { return jkx_ReadRange((const char*)ctx, off, out, n); }

typedef struct { const uint8_t* p; uint64_t n; } jkx_mem;
static int jkx_MemRead(void* ctx, uint64_t off, uint8_t* out, uint64_t n)
{
    jkx_mem* m = (jkx_mem*)ctx;
    if (off + n > m->n) return 0;
    memcpy(out, m->p + off, (size_t)n);
    return 1;
}

// Every file in the retail GOBs, and each whole GOB, found by size and
// SHA-256 (hashed only when a file of that size is looked for).
typedef struct { int file; uint32_t offset, size; int hashed; uint8_t sha[32]; } jkx_cand;

static struct
{
    char      paths[16][512];
    int       numPaths;
    jkx_cand* c;
    int       numC;
} XI;

static void jkx_IndexFree(void)
{
    free(XI.c);
    memset(&XI, 0, sizeof(XI));
}

static void jkx_IndexBuild(const char* ext)
{
    const char** list = !strcmp(ext, "goo") ? jkx_retailGoo : jkx_retailGob;
    int cap = 0;
    jkx_IndexFree();
    for (int i = 0; list[i] && XI.numPaths < 16; i++)
    {
        char path[512];
        uint64_t size;
        jkx_gobEntry* e;
        int n;
        if (!jkx_FindNoCase(list[i], path, sizeof(path)) || !jkx_IsFile(path, &size)) continue;
        n = jkx_ParseGob(jkx_FileRead, path, size, &e);
        if (n < 0) continue;
        snprintf(XI.paths[XI.numPaths], sizeof(XI.paths[0]), "%s", path);
        for (int k = 0; k <= n; k++)
        {
            uint32_t off = k < n ? e[k].offset : 0;
            uint64_t sz = k < n ? e[k].size : size;   // k == n: the whole GOB
            if (!sz || sz > 0xFFFFFFFFu) continue;
            if (XI.numC == cap)
            {
                int nc = cap ? cap * 2 : 4096;
                jkx_cand* c = (jkx_cand*)realloc(XI.c, (size_t)nc * sizeof(jkx_cand));
                if (!c) break;
                XI.c = c;
                cap = nc;
            }
            XI.c[XI.numC].file = XI.numPaths;
            XI.c[XI.numC].offset = off;
            XI.c[XI.numC].size = (uint32_t)sz;
            XI.c[XI.numC].hashed = 0;
            XI.numC++;
        }
        free(e);
        XI.numPaths++;
    }
}

static jkx_cand* jkx_IndexFind(uint32_t size, const uint8_t sha[32])
{
    for (int i = 0; i < XI.numC; i++)
    {
        jkx_cand* c = &XI.c[i];
        if (c->size != size) continue;
        if (!c->hashed)
        {
            if (!jkmod_Sha256FileRange(XI.paths[c->file], c->offset, c->size, c->sha)) continue;
            c->hashed = 1;
        }
        if (!memcmp(c->sha, sha, 32)) return c;
    }
    return NULL;
}

// rebuild(): the file from the stream and the joiner's own retail GOBs,
// checked against the host's SHA-256. Returns NULL and sets *reason on failure.
static uint8_t* jkx_Rebuild(const uint8_t* stream, uint32_t len, uint32_t size, uint32_t refsBytes,
                            const uint8_t sha[32], const char* ext, const char** reason)
{
    uint32_t count, src, pos = 0, prevOff = 0;
    uint8_t* out;
    uint8_t got[32];

    *reason = "corrupt";
    if (refsBytes < 4 || refsBytes > len) return NULL;
    count = rd32(stream);
    if (count > MAX_REFS || 4 + (uint64_t)count * REF_BYTES != refsBytes) return NULL;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t* r = stream + 4 + (size_t)i * REF_BYTES;
        uint32_t off = rd32(r), sz = rd32(r + 4);
        if (!sz || (uint64_t)off + sz > size || (i && off < prevOff)) return NULL;
        prevOff = off;
    }

    out = (uint8_t*)malloc(size ? size : 1);
    if (!out) { *reason = "install"; return NULL; }
    jkx_IndexBuild(ext);
    src = refsBytes;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t* r = stream + 4 + (size_t)i * REF_BYTES;
        uint32_t off = rd32(r), sz = rd32(r + 4);
        jkx_cand* hit;
        if (off > pos)
        {
            uint32_t n = off - pos;
            if ((uint64_t)src + n > len) { free(out); jkx_IndexFree(); return NULL; }
            memcpy(out + pos, stream + src, n);
            src += n;
        }
        hit = jkx_IndexFind(sz, r + 8);
        if (!hit || !jkx_ReadRange(XI.paths[hit->file], hit->offset, out + off, sz))
        {
            *reason = "missing-retail";
            free(out);
            jkx_IndexFree();
            return NULL;
        }
        if (off + sz > pos) pos = off + sz;
    }
    jkx_IndexFree();
    if (len - src != size - pos) { free(out); return NULL; }
    memcpy(out + pos, stream + src, len - src);

    jkmod_Sha256(out, size, got);
    if (memcmp(got, sha, 32)) { *reason = "mismatch"; free(out); return NULL; }
    *reason = NULL;
    return out;
}

// ---------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------

static int jkx_WriteFile(const char* path, const uint8_t* data, size_t n)
{
    FILE* f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(data, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (!ok) remove(path);
    return ok;
}

static const char* jkx_InstallMap(const uint8_t* data, uint32_t size)
{
    char dir[512], old[512], path[600], tmp[640];
    if (!jkx_FindNoCase("episode", dir, sizeof(dir)) || !jkx_IsDir(dir))
    {
        snprintf(dir, sizeof(dir), "episode");
        if (!jkx_MkDir(dir)) return "install";
    }
    snprintf(path, sizeof(path), "%s%s%s.%s", dir, JKX_SEP, X.name, X.ext);
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    if (!jkx_WriteFile(tmp, data, size)) return "install";

    // Keep a different copy of ours (whatever its case) as .bak.
    {
        char rel[128];
        snprintf(rel, sizeof(rel), "episode/%s.%s", X.name, X.ext);
        if (jkx_FindNoCase(rel, old, sizeof(old)) && jkx_IsFile(old, NULL))
        {
            char bak[640];
            snprintf(bak, sizeof(bak), "%s.bak", old);
            remove(bak);
            if (rename(old, bak) != 0) { remove(tmp); return "install"; }
            jkx_Log("kept your earlier %s.%s as %s", X.name, X.ext, bak);
        }
    }
    if (rename(tmp, path) != 0) { remove(tmp); return "install"; }
    jkx_Log("installed map %s.%s from the host", X.name, X.ext);
    return NULL;
}

// unpack(): checks every name, then writes the pack's files into a new
// folder under netmods/, and checks they give the host's fingerprint.
static const char* jkx_InstallMod(const uint8_t* data, uint32_t size)
{
    jkx_mem m = { data, size };
    jkx_gobEntry* e;
    int n = jkx_ParseGob(jkx_MemRead, &m, size, &e);
    char fp16[17], dir[512];
    uint8_t got[32];

    if (n < 0) return "corrupt";
    if (n > MAX_MOD_FILES) { free(e); return "too-many-files"; }
    for (int i = 0; i < n; i++)
    {
        if (!jkmod_ValidRel(e[i].name)) { free(e); return "bad-files"; }
        for (int k = 0; k < i; k++)
            if (jkx_EqNoCase(e[i].name, e[k].name)) { free(e); return "bad-files"; }
    }
    // A file and a folder can't share a name.
    for (int i = 0; i < n; i++)
    {
        size_t li = strlen(e[i].name);
        for (int k = 0; k < n; k++)
        {
            size_t lk = strlen(e[k].name);
            if (k != i && lk > li && e[k].name[li] == '/' && jkx_NEqNoCase(e[k].name, e[i].name, li)) { free(e); return "bad-files"; }
        }
    }

    jkx_Hex(X.fp, 8, fp16);
    if (!jkx_MkDir("netmods")) { free(e); return "install"; }
    snprintf(dir, sizeof(dir), "netmods%s%s", JKX_SEP, fp16);
    for (int k = 2; jkx_IsDir(dir) && k < 10; k++)   // a different folder already has the name
        snprintf(dir, sizeof(dir), "netmods%s%s-%d", JKX_SEP, fp16, k);
    if (jkx_IsDir(dir) || !jkx_MkDir(dir)) { free(e); return "install"; }

    for (int i = 0; i < n; i++)
    {
        char path[768];
        snprintf(path, sizeof(path), "%s%s%s", dir, JKX_SEP, e[i].name);
#ifdef _WIN32
        for (char* p = path; *p; p++) if (*p == '/') *p = '\\';
#endif
        if (!jkx_MkParents(path) || !jkx_WriteFile(path, data + e[i].offset, e[i].size)) { free(e); return "install"; }
    }
    free(e);

    if (!jkmod_Fingerprint(dir, got) || memcmp(got, X.fp, 32))
    {
        jkx_Log("mod unpacked into %s doesn't give the host's fingerprint", dir);
        return "install";
    }
    snprintf(X.resultPath, sizeof(X.resultPath), "%s", dir);
    jkx_Log("installed mod %s from the host (%d files) in %s", X.name, n, dir);
    return NULL;
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

static void jkx_Frame(uint8_t* f, int type)
{
    memset(f, 0, HEADER);
    wr32(f, 0);        // idFrom: the lobby stamps ours
    wr32(f + 4, 1);    // to the host
    memcpy(f + 8, "JKMT", 4);
    f[12] = (uint8_t)type;
    f[13] = VERSION;
    f[14] = (uint8_t)X.kind;
}

static void jkx_AskInfo(void)
{
    uint8_t f[HEADER];
    jkx_Frame(f, T_INFO_REQ);
    jkweb_XferSend(1, f, sizeof(f), jkweb_XferDirectUp());
    X.lastAsk = jkx_NowMs();
}

static void jkx_AskChunks(uint32_t first, uint32_t count)
{
    uint8_t f[HEADER + 20];
    uint64_t now = jkx_NowMs();
    int via;
    jkx_Frame(f, T_CHUNK_REQ);
    memcpy(f + HEADER, X.sha, 8);
    wr32(f + HEADER + 8, X.chunkSize);
    wr32(f + HEADER + 12, first);
    wr32(f + HEADER + 16, count);
    via = jkweb_XferSend(1, f, sizeof(f), jkweb_XferDirectUp());
    if (via) X.via = via;
    for (uint32_t i = first; i < first + count && i < X.n; i++)
    {
        if (!X.askedAt[i]) X.askedCount++;
        X.askedAt[i] = now ? now : 1;
    }
}

static void jkx_PumpRequests(void)
{
    while (X.next < X.n)
    {
        uint32_t count = X.n - X.next;
        uint32_t room = X.window > X.askedCount ? X.window - X.askedCount : 0;
        uint32_t minCount = X.batch < X.n - X.next ? X.batch : X.n - X.next;
        if (count > room) count = room;
        if (count > MAX_CHUNKS_PER_REQ) count = MAX_CHUNKS_PER_REQ;
        if (count < minCount) break;
        jkx_AskChunks(X.next, count);
        X.next += count;
    }
}

// "key": "value" (or a number) in the host's INFO JSON.
static const char* jkx_JsonFind(const char* json, const char* key)
{
    char pat[40];
    const char* p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static int jkx_JsonStr(const char* json, const char* key, char* out, size_t len)
{
    const char* p = jkx_JsonFind(json, key);
    size_t n = 0;
    if (!p || *p != '"') return 0;
    for (p++; *p && *p != '"'; p++)
    {
        char c = *p;
        if (c == '\\')
        {
            p++;
            if (!*p) return 0;
            c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'u' ? '?' : *p;
            if (*p == 'u') { for (int i = 0; i < 4 && p[1]; i++) p++; }
        }
        if (n + 1 < len) out[n++] = c;
    }
    out[n] = 0;
    return *p == '"';
}

static double jkx_JsonNum(const char* json, const char* key)
{
    const char* p = jkx_JsonFind(json, key);
    char* end;
    double v;
    if (!p || !(*p == '-' || isdigit((unsigned char)*p))) return NAN;
    v = strtod(p, &end);
    return end == p ? NAN : v;
}

static void jkx_OnInfo(const uint8_t* body, size_t n)
{
    char json[4096];
    if (n >= sizeof(json)) return;
    memcpy(json, body, n);
    json[n] = 0;
    if (!jkx_JsonStr(json, "status", X.infoStatus, sizeof(X.infoStatus))) return;
    if (!jkx_JsonStr(json, "name", X.infoName, sizeof(X.infoName))) X.infoName[0] = 0;
    if (!jkx_JsonStr(json, "ext", X.infoExt, sizeof(X.infoExt))) X.infoExt[0] = 0;
    if (!jkx_JsonStr(json, "sha256", X.infoSha, sizeof(X.infoSha))) X.infoSha[0] = 0;
    X.size = jkx_JsonNum(json, "size");
    X.streamSize = jkx_JsonNum(json, "streamSize");
    X.refsBytes = jkx_JsonNum(json, "refsBytes");
    X.z = jkx_JsonNum(json, "z");
    X.gotInfo = 1;
}

static void jkx_OnChunk(const uint8_t* p, size_t n)
{
    uint32_t i, len;
    if (n < HEADER + 12 || memcmp(p + HEADER, X.sha, 8)) return;
    i = rd32(p + HEADER + 8);
    if (i >= X.n || X.got[i]) return;
    len = X.total - i * X.chunkSize;
    if (len > X.chunkSize) len = X.chunkSize;
    if (n != (size_t)HEADER + 12 + len) return;
    memcpy(X.buf + (size_t)i * X.chunkSize, p + HEADER + 12, len);
    X.got[i] = 1;
    if (X.askedAt[i]) { X.askedAt[i] = 0; X.askedCount--; }
    X.received++;
    X.bytes += len;
    X.lastProgress = jkx_NowMs();
    if (X.received == X.n)
    {
        X.state = S_REBUILD;
        snprintf(X.status, sizeof(X.status), X.kind == JKX_KIND_MOD ? "%s: setting up the mod..." : "%s: putting the map together...", X.name);
    }
    else
        jkx_PumpRequests();
}

// Blocks being put together from their parts (a few are asked for at once).
#define JKX_BLOCK_SLOTS 8
static struct
{
    int      used;
    uint32_t block, zlen;
    int      codec;
    uint8_t* buf;
    uint8_t  units[BLOCK_BYTES / BLOCK_UNIT];
    uint32_t left;
} XB[JKX_BLOCK_SLOTS];

static void jkx_FreeBlocks(void)
{
    for (int i = 0; i < JKX_BLOCK_SLOTS; i++) free(XB[i].buf);
    memset(XB, 0, sizeof(XB));
}

static uint32_t jkx_BlockRaw(uint32_t b)
{
    uint32_t at = b * BLOCK_BYTES;
    return X.total - at < BLOCK_BYTES ? X.total - at : BLOCK_BYTES;
}

static void jkx_AskBlock(uint32_t b)
{
    uint8_t f[HEADER + 24];
    uint64_t now = jkx_NowMs();
    int via;
    jkx_Frame(f, T_BLOCK_REQ);
    memcpy(f + HEADER, X.sha, 8);
    wr32(f + HEADER + 8, b);
    wr32(f + HEADER + 12, 1);
    wr32(f + HEADER + 16, jkweb_XferDirectUp() ? PART_DIRECT : PART_RELAY);
#ifdef JKX_NO_ZLIB
    f[HEADER + 20] = CODEC_STORED;
#else
    f[HEADER + 20] = CODEC_DEFLATE;
#endif
    via = jkweb_XferSend(1, f, sizeof(f), jkweb_XferDirectUp());
    if (via) X.via = via;
    if (!X.askedAt[b]) X.askedCount++;
    X.askedAt[b] = now ? now : 1;
}

// One block at a time, as many out as the way takes.
static void jkx_PumpBlocks(void)
{
    uint32_t most = jkweb_XferDirectUp() ? BLOCKS_DIRECT : BLOCKS_RELAY;
    while (X.next < X.n && X.askedCount < most)
    {
        if (!X.got[X.next] && !X.askedAt[X.next]) jkx_AskBlock(X.next);
        X.next++;
    }
}

static void jkx_BlockDone(uint32_t b, const uint8_t* data)
{
    uint32_t raw = jkx_BlockRaw(b);
    memcpy(X.buf + (size_t)b * BLOCK_BYTES, data, raw);
    X.got[b] = 1;
    if (X.askedAt[b]) { X.askedAt[b] = 0; X.askedCount--; }
    X.received++;
    X.bytes += raw;
    if (X.received == X.n)
    {
        X.state = S_REBUILD;
        snprintf(X.status, sizeof(X.status), X.kind == JKX_KIND_MOD ? "%s: setting up the mod..." : "%s: putting the map together...", X.name);
    }
    else
        jkx_PumpBlocks();
}

static void jkx_OnBlock(const uint8_t* p, size_t n)
{
    uint32_t b, zlen, at, len, raw, units;
    int codec, slot = -1, free_ = -1;
    if (n < HEADER + BLOCK_HEADER || memcmp(p + HEADER, X.sha, 8)) return;
    b = rd32(p + HEADER + 8);
    zlen = rd32(p + HEADER + 12);
    at = rd32(p + HEADER + 16);
    codec = p[HEADER + 20];
    len = (uint32_t)(n - HEADER - BLOCK_HEADER);
    if (b >= X.n || X.got[b]) return;
    raw = jkx_BlockRaw(b);
#ifdef JKX_NO_ZLIB
    if (codec != CODEC_STORED) return;
#endif
    if (codec == CODEC_STORED ? zlen != raw : (codec != CODEC_DEFLATE || !zlen || zlen >= raw)) return;
    if (at % BLOCK_UNIT || !len || at + len > zlen || (len % BLOCK_UNIT && at + len != zlen)) return;
    for (int i = 0; i < JKX_BLOCK_SLOTS; i++)
    {
        if (XB[i].used && XB[i].block == b) slot = i;
        if (free_ < 0 && !XB[i].used) free_ = i;
    }
    if (slot >= 0 && (XB[slot].zlen != zlen || XB[slot].codec != codec))
    {
        free(XB[slot].buf);
        memset(&XB[slot], 0, sizeof(XB[slot]));
        free_ = slot;
        slot = -1;
    }
    if (slot < 0)
    {
        if (free_ < 0) return;   // more blocks at once than asked for
        slot = free_;
        if (!(XB[slot].buf = (uint8_t*)malloc(zlen))) return;
        XB[slot].used = 1;
        XB[slot].block = b;
        XB[slot].zlen = zlen;
        XB[slot].codec = codec;
        XB[slot].left = (zlen + BLOCK_UNIT - 1) / BLOCK_UNIT;
    }
    memcpy(XB[slot].buf + at, p + HEADER + BLOCK_HEADER, len);
    units = (at + len + BLOCK_UNIT - 1) / BLOCK_UNIT;
    for (uint32_t u = at / BLOCK_UNIT; u < units; u++)
        if (!XB[slot].units[u]) { XB[slot].units[u] = 1; XB[slot].left--; }
    X.lastProgress = jkx_NowMs();
    if (X.askedAt[b]) X.askedAt[b] = X.lastProgress;
    if (XB[slot].left) return;

    if (codec == CODEC_STORED)
        jkx_BlockDone(b, XB[slot].buf);
#ifndef JKX_NO_ZLIB
    else
    {
        uint8_t* out = (uint8_t*)malloc(raw);
        z_stream zs;
        int ok = 0;
        memset(&zs, 0, sizeof(zs));
        if (out && inflateInit2(&zs, -15) == Z_OK)
        {
            zs.next_in = XB[slot].buf;
            zs.avail_in = zlen;
            zs.next_out = out;
            zs.avail_out = raw;
            ok = inflate(&zs, Z_FINISH) == Z_STREAM_END && zs.total_out == raw;
            inflateEnd(&zs);
        }
        if (ok) jkx_BlockDone(b, out);
        free(out);
        if (!ok)
        {
            free(XB[slot].buf);
            memset(&XB[slot], 0, sizeof(XB[slot]));
            jkx_Fail("corrupt");
            return;
        }
    }
#endif
    free(XB[slot].buf);
    memset(&XB[slot], 0, sizeof(XB[slot]));
}

static void jkx_OnHostFrame(uint32_t from, const uint8_t* p, size_t n, int via);

void jkx_OnFrame(uint32_t from, const uint8_t* p, size_t n, int via)
{
    if (n < HEADER || memcmp(p + 8, "JKMT", 4)) return;
    if (p[13] != VERSION || (p[14] != JKX_KIND_MAP && p[14] != JKX_KIND_MOD)) return;
    if (p[12] == T_INFO_REQ || p[12] == T_CHUNK_REQ || p[12] == T_BLOCK_REQ)
    {
        jkx_OnHostFrame(from, p, n, via);
        return;
    }
    if (p[14] != X.kind || from != 1 || X.result != JKX_BUSY) return;
    if (p[12] == T_INFO && X.state == S_INFO && !X.gotInfo)
        jkx_OnInfo(p + HEADER, n - HEADER);
    else if (p[12] == T_CHUNK && X.state == S_DOWNLOAD && !X.blocks)
        jkx_OnChunk(p, n);
    else if (p[12] == T_BLOCK && X.state == S_DOWNLOAD && X.blocks)
        jkx_OnBlock(p, n);
}

// ---------------------------------------------------------------------------
// The job
// ---------------------------------------------------------------------------

static void jkx_Reset(int kind, const char* name, const char* ext)
{
    jkx_FreeDownload();
    memset(&X, 0, sizeof(X));
    X.kind = kind;
    X.result = JKX_BUSY;
    snprintf(X.name, sizeof(X.name), "%s", name ? name : "");
    snprintf(X.ext, sizeof(X.ext), "%s", ext ? ext : "");
    X.started = jkx_NowMs();
    X.state = S_INFO;
    snprintf(X.status, sizeof(X.status), kind == JKX_KIND_MOD ? "Checking the host's mod..." : "Checking the host's map...");
}

void jkx_StartMap(const char* name, const char* ext)
{
    char rel[128];
    jkx_Reset(JKX_KIND_MAP, name, ext);
    if (!jkx_ValidEpisodeName(X.name) || (strcmp(X.ext, "gob") && strcmp(X.ext, "goo"))) { jkx_Fail("bad-name"); return; }
    snprintf(rel, sizeof(rel), "episode/%s.%s", X.name, X.ext);
    X.haveLocal = jkx_FindNoCase(rel, X.localPath, sizeof(X.localPath)) && jkx_IsFile(X.localPath, NULL);
    if (jkx_IsRetailEpisode(X.name))
    {
        if (X.haveLocal) jkx_Ready();
        else jkx_Fail("retail-missing");
        return;
    }
    jkx_AskInfo();
}

// Added: for the join screen: 0 an original episode (or a name that can't be a map), 1 a
// custom map with a copy in episode/ (checked against the host's when joining), 2 one to download.
int jkx_MapState(const char* name, const char* ext)
{
    char rel[128], path[512];
    if (!name || !jkx_ValidEpisodeName(name) || jkx_IsRetailEpisode(name)) return 0;
    snprintf(rel, sizeof(rel), "episode/%s.%s", name, ext);
    return jkx_FindNoCase(rel, path, sizeof(path)) && jkx_IsFile(path, NULL) ? 1 : 2;
}

void jkx_StartMod(const uint8_t fp[32], const char* name, const char* ext)
{
    jkx_Reset(JKX_KIND_MOD, name, ext);
    memcpy(X.fp, fp, 32);
    jkx_AskInfo();
}

void jkx_Cancel(void)
{
    jkx_Fail("cancelled");
}

void jkx_SetAskFirst(int bAsk)
{
    jkx_bAskFirst = bAsk;
}

const char* jkx_AskText(void) { return X.ask; }

void jkx_Answer(int bYes)
{
    if (X.result != JKX_BUSY || X.state != S_ASK) return;
    if (!bYes) { jkx_Fail("declined"); return; }
    X.state = S_WAIT_DIRECT;
    X.phaseStart = jkx_NowMs();
    snprintf(X.status, sizeof(X.status), "Connecting to the host...");
}

static int jkx_ShaHex(const char* hex, uint8_t out[32])
{
    if (strlen(hex) != 64) return 0;
    for (int i = 0; i < 64; i++)
        if (!((hex[i] >= '0' && hex[i] <= '9') || (hex[i] >= 'a' && hex[i] <= 'f'))) return 0;
    for (int i = 0; i < 32; i++)
    {
        int hi = hex[2*i] <= '9' ? hex[2*i] - '0' : hex[2*i] - 'a' + 10;
        int lo = hex[2*i+1] <= '9' ? hex[2*i+1] - '0' : hex[2*i+1] - 'a' + 10;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 1;
}

// checkInfo(): sizes and the hash have to make sense before anything is asked for.
static int jkx_CheckInfo(double maxBytes)
{
    if (!(X.size > 0 && X.size <= maxBytes)) return jkx_Fail("too-big");
    if (!(X.refsBytes >= 4 && X.streamSize >= X.refsBytes && X.streamSize <= X.size + 4 + (double)MAX_REFS * REF_BYTES)
        || floor(X.size) != X.size || floor(X.streamSize) != X.streamSize || floor(X.refsBytes) != X.refsBytes
        || !jkx_ShaHex(X.infoSha, X.sha))
        return jkx_Fail("corrupt");
    return JKX_BUSY;
}

static int jkx_HandleInfo(void)
{
    if (strcmp(X.infoStatus, "ok")) return jkx_Fail(X.infoStatus);
    if (X.kind == JKX_KIND_MAP)
    {
        uint64_t localSize;
        if (!jkx_EqNoCase(X.infoName, X.name)) return jkx_Fail("other-map");
        if (strcmp(X.infoExt, X.ext)) return jkx_Fail("other-game");
        if (jkx_CheckInfo(MAX_MAP_BYTES) != JKX_BUSY) return X.result;
        if (X.haveLocal && jkx_IsFile(X.localPath, &localSize) && (double)localSize == X.size)
        {
            uint8_t sha[32];
            if (jkmod_Sha256FileRange(X.localPath, 0, localSize, sha) && !memcmp(sha, X.sha, 32))
                return jkx_Ready();
        }
    }
    else
    {
        if (strcmp(X.infoExt, X.ext)) return jkx_Fail("other-game");
        if (jkx_CheckInfo(MAX_SEND_BYTES) != JKX_BUSY) return X.result;
        if (memcmp(X.sha, X.fp, 32)) return jkx_Fail("other-mod");
    }
    if (jkx_bAskFirst)
    {
        char mb[16];
        jkx_Mb(mb, sizeof(mb), X.streamSize);
        if (X.kind == JKX_KIND_MOD)
            snprintf(X.ask, sizeof(X.ask), "The host runs the mod \"%s\", which you don't have. "
                     "Download it from the host (%s MB)? It goes in your netmods folder.", X.name, mb);
        else if (X.haveLocal)
            snprintf(X.ask, sizeof(X.ask), "Your copy of the map \"%s\" is different from the host's. "
                     "Download theirs (%s MB)? Yours is kept as %s.%s.bak.", X.name, mb, X.name, X.ext);
        else
            snprintf(X.ask, sizeof(X.ask), "The host runs the map \"%s\", which you don't have. "
                     "Download it from the host (%s MB)? It goes in your episode folder.", X.name, mb);
        snprintf(X.status, sizeof(X.status), "Waiting for your answer...");
        X.state = S_ASK;
        return JKX_ASK;
    }
    X.state = S_WAIT_DIRECT;
    X.phaseStart = jkx_NowMs();
    snprintf(X.status, sizeof(X.status), "Connecting to the host...");
    return JKX_BUSY;
}

static int jkx_BeginDownload(void)
{
    // Added: a host that says z sends blocks (maps.js since 2026-10-29)
    X.blocks = X.z == BLOCK_BYTES;
    X.chunkSize = X.blocks ? BLOCK_BYTES : jkweb_XferDirectUp() ? CHUNK_DIRECT : CHUNK_RELAY;
    X.total = (uint32_t)X.streamSize;
    X.n = (X.total + X.chunkSize - 1) / X.chunkSize;
    X.buf = (uint8_t*)malloc(X.total);
    X.got = (uint8_t*)calloc(X.n, 1);
    X.askedAt = (uint64_t*)calloc(X.n, sizeof(uint64_t));
    if (!X.buf || !X.got || !X.askedAt) return jkx_Fail("install");
    X.window = WINDOW_BYTES / X.chunkSize;
    if (X.window < 4) X.window = 4;
    // Half a window per request keeps requests to a few per window: through
    // the lobby each one counts against its daily limit.
    X.batch = X.window / 2 ? X.window / 2 : 1;
    X.lastProgress = X.lastResendCheck = jkx_NowMs();
    X.via = jkweb_XferDirectUp() ? JKX_VIA_DIRECT : JKX_VIA_RELAY;
    X.state = S_DOWNLOAD;
    if (X.blocks) jkx_PumpBlocks();
    else jkx_PumpRequests();
    return JKX_BUSY;
}

static void jkx_Resend(uint64_t now)
{
    if (X.blocks)
    {
        // A block that hasn't moved for a while is asked for again (what came of it is kept).
        for (uint32_t b = 0; b < X.n; b++)
            if (X.askedAt[b] && now - X.askedAt[b] > CHUNK_RESEND_MS) jkx_AskBlock(b);
        jkx_PumpBlocks();
        return;
    }
    uint32_t first = 0, count = 0;
    for (uint32_t i = 0; i <= X.n; i++)
    {
        int late = i < X.n && X.askedAt[i] && now - X.askedAt[i] > CHUNK_RESEND_MS;
        if (late && count && first + count == i && count < MAX_CHUNKS_PER_REQ) { count++; continue; }
        if (count) jkx_AskChunks(first, count);
        count = 0;
        if (late) { first = i; count = 1; }
    }
}

int jkx_Tick(void)
{
    uint64_t now;
    int hs;
    if (X.result != JKX_BUSY) return X.result;

    jkweb_XferPump();
    if (X.result != JKX_BUSY) return X.result;
    hs = jkweb_XferHostState();
    if (hs == 0) return jkx_Fail("host-left");
    if (hs < 0) return jkx_Fail("lobby-lost");
    now = jkx_NowMs();

    switch (X.state)
    {
    case S_INFO:
        if (X.gotInfo) return jkx_HandleInfo();
        if (now - X.started > (X.haveLocal ? INFO_TIMEOUT_LOCAL_MS : INFO_TIMEOUT_MS))
        {
            // A host that doesn't serve maps never answers; a player who has
            // the map could always join it.
            if (X.kind == JKX_KIND_MAP && X.haveLocal)
            {
                jkx_Log("the host didn't answer about its map, using your own %s.%s", X.name, X.ext);
                return jkx_Ready();
            }
            return jkx_Fail("no-answer");
        }
        if (now - X.lastAsk >= INFO_RESEND_MS) jkx_AskInfo();
        break;

    case S_ASK:
        return JKX_ASK;

    case S_WAIT_DIRECT:
        if (jkweb_XferDirectUp() || !jkweb_XferDirectPossible() || now - X.phaseStart >= DIRECT_WAIT_MS)
            return jkx_BeginDownload();
        break;

    case S_DOWNLOAD:
    {
        char a[16], b[16];
        if (now - X.lastProgress > STALL_MS) return jkx_Fail("stalled");
        if (now - X.lastResendCheck >= 500)
        {
            X.lastResendCheck = now;
            jkx_Resend(now);
        }
        jkx_Mb(a, sizeof(a), (double)X.bytes);
        jkx_Mb(b, sizeof(b), (double)X.total);
        snprintf(X.status, sizeof(X.status), "%s: %s of %s MB, %s", X.name, a, b,
                 X.via == JKX_VIA_DIRECT ? "straight from the host" : "through the lobby");
        break;
    }

    case S_REBUILD:
    {
        const char* reason;
        uint8_t* file = jkx_Rebuild(X.buf, X.total, (uint32_t)X.size, (uint32_t)X.refsBytes, X.sha, X.ext, &reason);
        if (!file) return jkx_Fail(reason);
        reason = X.kind == JKX_KIND_MOD ? jkx_InstallMod(file, (uint32_t)X.size) : jkx_InstallMap(file, (uint32_t)X.size);
        free(file);
        if (reason) return jkx_Fail(reason);
        return jkx_Ready();
    }
    }
    return X.result;
}

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------

typedef struct
{
    int      active;
    char     json[700];      // the INFO joiners get
    uint8_t* stream;         // with status "ok": what the chunks cut up
    uint32_t streamLen;
    uint8_t  id[8];
    // Blocks, deflated as they're asked for and kept for the next joiner: aZLen 0 not
    // yet, else the size (the block's own size: it doesn't get smaller, sent as it is).
    uint32_t numBlocks;
    uint8_t** aZ;
    uint32_t* aZLen;
} jkx_offer;

static jkx_offer XO[2];   // by kind

// Chunks joiners asked for, sent a few at a time by jkx_HostPump() while the
// link has room: a request is up to 128 chunks (6 MiB through the lobby), too
// much to write in one go without holding up the game.
#define JKX_HOST_QUEUE 64
#define JKX_HOST_FRAMES_PER_PUMP 4
// Chunks (size: the chunk size), or (block set) parts of a block: next is the
// offset of the next part, end the block's size as sent.
typedef struct { uint32_t to; int kind, direct; uint32_t next, end, size; int block; uint32_t b; int accept; } jkx_hostReq;
static jkx_hostReq XQ[JKX_HOST_QUEUE];
static int XQn;

static void jkx_HostFrame(uint8_t* f, int type, uint32_t to, int kind);

typedef struct { uint32_t off, size; uint8_t sha[32]; } jkx_ref;

static int jkx_CmpRef(const void* a, const void* b)
{
    const jkx_ref* x = (const jkx_ref*)a;
    const jkx_ref* y = (const jkx_ref*)b;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    if (x->size != y->size) return x->size > y->size ? -1 : 1;
    return 0;
}

// planShareRanges(): each range that is a copy of a retail file becomes a
// reference; the stream is the reference table and the bytes outside them.
// NULL, or the reason it can't be shared.
static const char* jkx_PlanShare(const uint8_t* bytes, uint32_t len, const jkx_gobEntry* ranges, int numRanges,
                                 const char* ext, uint8_t** outStream, uint32_t* outLen, uint32_t* outRefsBytes, int* outRefs)
{
    jkx_ref* refs = (jkx_ref*)malloc(sizeof(jkx_ref) * (size_t)(numRanges ? numRanges : 1));
    int numRefs = 0, custom = 0, kept = 0;
    uint64_t inlineLen = 0, end = 0, pos = 0, at;
    uint8_t* stream;
    if (!refs) return "error";

    jkx_IndexBuild(ext);
    for (int i = 0; i < numRanges; i++)
    {
        const jkx_gobEntry* e = &ranges[i];
        int hasSize = 0;
        uint8_t sha[32];
        if (!e->size) continue;
        for (int k = 0; k < XI.numC && !hasSize; k++) hasSize = XI.c[k].size == e->size;
        if (hasSize)
        {
            jkmod_Sha256(bytes + e->offset, e->size, sha);
            if (jkx_IndexFind(e->size, sha))
            {
                refs[numRefs].off = e->offset;
                refs[numRefs].size = e->size;
                memcpy(refs[numRefs].sha, sha, 32);
                numRefs++;
                continue;
            }
        }
        custom++;
    }
    jkx_IndexFree();
    if (!custom) { free(refs); return "retail-only"; }

    // A reference inside another adds nothing.
    qsort(refs, (size_t)numRefs, sizeof(jkx_ref), jkx_CmpRef);
    for (int i = 0; i < numRefs; i++)
    {
        if (kept && (uint64_t)refs[i].off + refs[i].size <= end) continue;
        refs[kept++] = refs[i];
        if ((uint64_t)refs[i].off + refs[i].size > end) end = (uint64_t)refs[i].off + refs[i].size;
    }
    numRefs = kept;
    if (numRefs > MAX_REFS) { free(refs); return "too-many-files"; }

    for (int i = 0; i < numRefs; i++)
    {
        if (refs[i].off > pos) inlineLen += refs[i].off - pos;
        if ((uint64_t)refs[i].off + refs[i].size > pos) pos = (uint64_t)refs[i].off + refs[i].size;
    }
    if (pos < len) inlineLen += len - pos;

    *outRefsBytes = 4 + (uint32_t)numRefs * REF_BYTES;
    *outLen = (uint32_t)(*outRefsBytes + inlineLen);
    stream = (uint8_t*)malloc(*outLen);
    if (!stream) { free(refs); return "error"; }
    wr32(stream, (uint32_t)numRefs);
    for (int i = 0; i < numRefs; i++)
    {
        wr32(stream + 4 + (size_t)i * REF_BYTES, refs[i].off);
        wr32(stream + 8 + (size_t)i * REF_BYTES, refs[i].size);
        memcpy(stream + 12 + (size_t)i * REF_BYTES, refs[i].sha, 32);
    }
    at = *outRefsBytes;
    pos = 0;
    for (int i = 0; i < numRefs; i++)
    {
        if (refs[i].off > pos)
        {
            memcpy(stream + at, bytes + pos, refs[i].off - pos);
            at += refs[i].off - pos;
        }
        if ((uint64_t)refs[i].off + refs[i].size > pos) pos = (uint64_t)refs[i].off + refs[i].size;
    }
    if (pos < len) memcpy(stream + at, bytes + pos, len - pos);
    free(refs);
    *outStream = stream;
    *outRefs = numRefs;
    return NULL;
}

// A JSON string literal (names come from file names or the player).
static void jkx_JsonQuote(const char* in, char* out, size_t len)
{
    size_t n = 0;
    if (len < 3) { if (len) out[0] = 0; return; }
    out[n++] = '"';
    for (; *in && n + 8 < len; in++)
    {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20 || c > 0x7e) n += (size_t)snprintf(out + n, len - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n++] = '"';
    out[n] = 0;
}

static void jkx_OfferInfo(jkx_offer* o, const char* status, const char* name, const char* ext)
{
    char qs[64], qn[200], qe[32];
    jkx_JsonQuote(status, qs, sizeof(qs));
    jkx_JsonQuote(name, qn, sizeof(qn));
    jkx_JsonQuote(ext, qe, sizeof(qe));
    snprintf(o->json, sizeof(o->json), "{\"status\":%s,\"name\":%s,\"ext\":%s}", qs, qn, qe);
}

static void jkx_OfferOk(jkx_offer* o, const char* name, const char* ext, const uint8_t* bytes, uint32_t len,
                        uint8_t* stream, uint32_t streamLen, uint32_t refsBytes)
{
    char qn[200], qe[32], hex[65];
    uint8_t sha[32];
    jkmod_Sha256(bytes, len, sha);
    jkx_Hex(sha, 32, hex);
    jkx_JsonQuote(name, qn, sizeof(qn));
    jkx_JsonQuote(ext, qe, sizeof(qe));
    snprintf(o->json, sizeof(o->json),
             "{\"status\":\"ok\",\"name\":%s,\"ext\":%s,\"size\":%u,\"sha256\":\"%s\",\"streamSize\":%u,\"refsBytes\":%u,\"z\":%d}",
             qn, qe, len, hex, streamLen, refsBytes, BLOCK_BYTES);
    o->numBlocks = (streamLen + BLOCK_BYTES - 1) / BLOCK_BYTES;
    o->aZ = (uint8_t**)calloc(o->numBlocks ? o->numBlocks : 1, sizeof(uint8_t*));
    o->aZLen = (uint32_t*)calloc(o->numBlocks ? o->numBlocks : 1, sizeof(uint32_t));
    o->stream = stream;
    o->streamLen = streamLen;
    memcpy(o->id, sha, 8);
}

// maps.js host(): our episode GOB, unless it's an original one.
static void jkx_OfferMap(const char* name, const char* ext)
{
    jkx_offer* o = &XO[JKX_KIND_MAP];
    char rel[128], path[512];
    uint64_t size;
    int line;
    jkx_OfferInfo(o, "none", name, ext);
    o->active = 1;
    snprintf(rel, sizeof(rel), "episode/%s.%s", name, ext);
    if (jkx_IsRetailEpisode(name))
        jkx_OfferInfo(o, "retail", name, ext);
    else if ((line = jkx_NoShare(rel)))
    {
        jkx_OfferInfo(o, "not-shared", name, ext);
        jkx_Log("map %s.%s won't be sent to joiners: " JKX_NOSHARE_FILE " line %d", name, ext, line);
        return;
    }
    else if (jkx_ValidEpisodeName(name) && (!strcmp(ext, "gob") || !strcmp(ext, "goo"))
             && jkx_FindNoCase(rel, path, sizeof(path)) && jkx_IsFile(path, &size))
    {
        if (size > MAX_MAP_BYTES)
            jkx_OfferInfo(o, "too-big", name, ext);
        else
        {
            uint8_t* gob = (uint8_t*)malloc(size ? (size_t)size : 1);
            jkx_gobEntry* e = NULL;
            jkx_mem m;
            int n;
            if (!gob || !jkx_ReadRange(path, 0, gob, size)) { free(gob); jkx_OfferInfo(o, "error", name, ext); return; }
            m.p = gob; m.n = size;
            n = jkx_ParseGob(jkx_MemRead, &m, size, &e);
            if (n < 0)
                jkx_OfferInfo(o, "not-gob", name, ext);
            else
            {
                uint8_t* stream;
                uint32_t streamLen, refsBytes;
                int refs;
                const char* why = jkx_PlanShare(gob, (uint32_t)size, e, n, ext, &stream, &streamLen, &refsBytes, &refs);
                if (why)
                    jkx_OfferInfo(o, why, name, ext);
                else
                {
                    jkx_OfferOk(o, name, ext, gob, (uint32_t)size, stream, streamLen, refsBytes);
                    jkx_Log("ready to send map %s.%s to joiners (%d original files left out, %u of %u bytes to send)",
                            name, ext, refs, streamLen, (uint32_t)size);
                }
            }
            free(e);
            free(gob);
        }
    }
    if (!o->stream) jkx_Log("map %s.%s won't be sent to joiners: %s", name, ext, o->json);
}

// mods.js host(): the running mod's pack, checked against the fingerprint
// in our lobby tail.
static void jkx_OfferMod(const char* path, int isFile, const uint8_t fp[32], const char* name, const char* ext)
{
    jkx_offer* o = &XO[JKX_KIND_MOD];
    uint8_t* pack;
    uint32_t packLen;
    jkmod_packEntry* pe;
    int n, numRanges = 0;
    jkx_gobEntry* ranges;
    const char* why;
    uint8_t sha[32];
    int line = 0;

    o->active = 1;
    jkx_OfferInfo(o, "error", name, ext);
    if (isFile == JKMOD_SRC_ALWAYS_JK || isFile == JKMOD_SRC_ALWAYS_MOTS)
    {
        // Added: several always-loaded GOBs as one mod: held back if any of them is listed.
        char one[512];
        for (int i = 0; !line && jkmod_GetAlwaysLoaded(isFile == JKMOD_SRC_ALWAYS_MOTS, i, one, sizeof(one)); i++)
            line = jkx_NoShare(one);
    }
    else
        line = jkx_NoShare(path);
    if (line)
    {
        jkx_OfferInfo(o, "not-shared", name, ext);
        jkx_Log("mod %s won't be sent to joiners: " JKX_NOSHARE_FILE " line %d", name, line);
        return;
    }
    why = jkmod_Pack(path, isFile, MAX_MOD_FILES, MAX_MOD_BYTES, &pack, &packLen, &pe, &n);
    if (why) { jkx_OfferInfo(o, why, name, ext); jkx_Log("mod %s won't be sent to joiners: %s", name, why); return; }
    jkmod_Sha256(pack, packLen, sha);
    if (memcmp(sha, fp, 32))
    {
        jkx_OfferInfo(o, "changed", name, ext);
        jkx_Log("mod %s won't be sent to joiners: its files changed since it was fingerprinted", name);
        free(pack); free(pe);
        return;
    }

    // shareRanges(): each file, and each file inside a GOB the mod carries.
    {
        int cap = n + 16;
        ranges = (jkx_gobEntry*)malloc(sizeof(jkx_gobEntry) * (size_t)cap);
        for (int i = 0; ranges && i < n; i++)
        {
            size_t nl = strlen(pe[i].name);
            ranges[numRanges].offset = pe[i].offset;
            ranges[numRanges].size = pe[i].size;
            ranges[numRanges].name[0] = 0;
            numRanges++;
            if (nl >= 4 && (jkx_EqNoCase(pe[i].name + nl - 4, ".gob") || jkx_EqNoCase(pe[i].name + nl - 4, ".goo")))
            {
                jkx_mem m = { pack + pe[i].offset, pe[i].size };
                jkx_gobEntry* inner;
                int k = jkx_ParseGob(jkx_MemRead, &m, pe[i].size, &inner);
                if (k > 0)
                {
                    jkx_gobEntry* nr = (jkx_gobEntry*)realloc(ranges, sizeof(jkx_gobEntry) * (size_t)(cap + k));
                    if (nr)
                    {
                        ranges = nr;
                        cap += k;
                        for (int j = 0; j < k; j++)
                        {
                            ranges[numRanges].offset = pe[i].offset + inner[j].offset;
                            ranges[numRanges].size = inner[j].size;
                            ranges[numRanges].name[0] = 0;
                            numRanges++;
                        }
                    }
                }
                free(inner);
            }
        }
    }
    if (!ranges) { free(pack); free(pe); return; }

    {
        uint8_t* stream;
        uint32_t streamLen, refsBytes;
        int refs;
        why = jkx_PlanShare(pack, packLen, ranges, numRanges, ext, &stream, &streamLen, &refsBytes, &refs);
        if (why)
        {
            jkx_OfferInfo(o, why, name, ext);
            jkx_Log("mod %s won't be sent to joiners: %s", name, why);
        }
        else
        {
            jkx_OfferOk(o, name, ext, pack, packLen, stream, streamLen, refsBytes);
            jkx_Log("ready to send mod %s to joiners (%d files, %d parts left out as original game files, %u of %u bytes to send)",
                    name, n, refs, streamLen, packLen);
        }
    }
    free(ranges);
    free(pack);
    free(pe);
}

void jkx_HostWithdraw(void)
{
    for (int k = 0; k < 2; k++)
    {
        for (uint32_t b = 0; XO[k].aZ && b < XO[k].numBlocks; b++) free(XO[k].aZ[b]);
        free(XO[k].aZ);
        free(XO[k].aZLen);
        free(XO[k].stream);
        memset(&XO[k], 0, sizeof(XO[k]));
    }
    XQn = 0;
}

// Block b of offer o as a joiner that can inflate (accept) gets it: its bytes and
// size, and whether they're deflated.
static const uint8_t* jkx_HostBlock(jkx_offer* o, uint32_t b, int accept, uint32_t* pLen, int* pCodec)
{
    uint32_t at = b * BLOCK_BYTES;
    uint32_t raw = o->streamLen - at < BLOCK_BYTES ? o->streamLen - at : BLOCK_BYTES;
    *pLen = raw;
    *pCodec = CODEC_STORED;
#ifndef JKX_NO_ZLIB
    if ((accept & CODEC_DEFLATE) && o->aZLen)
    {
        if (!o->aZLen[b])
        {
            uLongf zn = compressBound(raw);
            uint8_t* z = (uint8_t*)malloc(zn);
            z_stream zs;
            memset(&zs, 0, sizeof(zs));
            o->aZLen[b] = raw;   // unless it gets smaller
            if (z && deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK)
            {
                zs.next_in = o->stream + at;
                zs.avail_in = raw;
                zs.next_out = z;
                zs.avail_out = (uInt)zn;
                if (deflate(&zs, Z_FINISH) == Z_STREAM_END && zs.total_out < raw)
                {
                    o->aZ[b] = z;
                    o->aZLen[b] = (uint32_t)zs.total_out;
                    z = NULL;
                }
                deflateEnd(&zs);
            }
            free(z);
        }
        if (o->aZ[b])
        {
            *pLen = o->aZLen[b];
            *pCodec = CODEC_DEFLATE;
            return o->aZ[b];
        }
    }
#else
    (void)accept;
#endif
    return o->stream + at;
}

void jkx_HostPump(void)
{
    static uint8_t f[HEADER + BLOCK_HEADER + PART_DIRECT];
    int frames = 0;
    while (XQn && frames < JKX_HOST_FRAMES_PER_PUMP)
    {
        jkx_hostReq* q = &XQ[0];
        jkx_offer* o = &XO[q->kind];
        uint32_t len;
        if (q->block)
        {
            // Added: parts of a block (maps.js sendBlock)
            uint32_t zlen;
            int codec;
            const uint8_t* z;
            if (!o->stream || q->b >= o->numBlocks || (q->end && q->next >= q->end))
            {
                memmove(XQ, XQ + 1, sizeof(XQ[0]) * (size_t)--XQn);
                continue;
            }
            if (!jkweb_XferCanSend(q->to, q->direct)) break;
            z = jkx_HostBlock(o, q->b, q->accept, &zlen, &codec);
            q->end = zlen;
            len = zlen - q->next < q->size ? zlen - q->next : q->size;
            jkx_HostFrame(f, T_BLOCK, q->to, q->kind);
            memcpy(f + HEADER, o->id, 8);
            wr32(f + HEADER + 8, q->b);
            wr32(f + HEADER + 12, zlen);
            wr32(f + HEADER + 16, q->next);
            memset(f + HEADER + 20, 0, 4);
            f[HEADER + 20] = (uint8_t)codec;
            memcpy(f + HEADER + BLOCK_HEADER, z + q->next, len);
            if (!jkweb_XferSend(q->to, f, HEADER + BLOCK_HEADER + len, q->direct))
            {
                memmove(XQ, XQ + 1, sizeof(XQ[0]) * (size_t)--XQn);
                continue;
            }
            q->next += len;
            frames++;
            continue;
        }
        if (!o->stream || q->next >= q->end || (uint64_t)q->next * q->size >= o->streamLen)
        {
            memmove(XQ, XQ + 1, sizeof(XQ[0]) * (size_t)--XQn);
            continue;
        }
        if (!jkweb_XferCanSend(q->to, q->direct)) break;
        len = o->streamLen - q->next * q->size;
        if (len > q->size) len = q->size;
        jkx_HostFrame(f, T_CHUNK, q->to, q->kind);
        memcpy(f + HEADER, o->id, 8);
        wr32(f + HEADER + 8, q->next);
        memcpy(f + HEADER + 12, o->stream + (size_t)q->next * q->size, len);
        if (!jkweb_XferSend(q->to, f, HEADER + 12 + len, q->direct))
        {
            // The joiner asks again for what doesn't come.
            memmove(XQ, XQ + 1, sizeof(XQ[0]) * (size_t)--XQn);
            continue;
        }
        q->next++;
        frames++;
    }
}

void jkx_HostOffer(const char* mapName, const char* ext, const char* modPath, int modIsFile,
                   const uint8_t* modFp, const char* modName)
{
    jkx_HostWithdraw();
    if (mapName && mapName[0]) jkx_OfferMap(mapName, ext);
    // Without a mod, mods.js answers no mod requests at all.
    if (modPath && modPath[0] && modFp) jkx_OfferMod(modPath, modIsFile, modFp, modName ? modName : "mod", ext);
}

static void jkx_HostFrame(uint8_t* f, int type, uint32_t to, int kind)
{
    memset(f, 0, HEADER);
    wr32(f, 1);        // from the host
    wr32(f + 4, to);
    memcpy(f + 8, "JKMT", 4);
    f[12] = (uint8_t)type;
    f[13] = VERSION;
    f[14] = (uint8_t)kind;
}

// onHostFrame(): answers on the way the request came.
static void jkx_OnHostFrame(uint32_t from, const uint8_t* p, size_t n, int via)
{
    int direct = via != JKX_VIA_RELAY;
    if (from < 2 || from > 64) return;
    if (p[12] == T_INFO_REQ)
    {
        jkx_offer* o = &XO[p[14]];
        static uint8_t f[HEADER + sizeof(o->json)];
        size_t l;
        if (!o->active) return;
        l = strlen(o->json);
        jkx_HostFrame(f, T_INFO, from, p[14]);
        memcpy(f + HEADER, o->json, l);
        jkweb_XferSend(from, f, HEADER + l, direct);
    }
    else if (p[12] == T_CHUNK_REQ && n >= HEADER + 20)
    {
        // Chunks are asked for by the file's id, whichever kind it is.
        jkx_offer* o = NULL;
        int k;
        uint32_t size = rd32(p + HEADER + 8), first = rd32(p + HEADER + 12), count = rd32(p + HEADER + 16);
        for (k = 0; k < 2; k++)
            if (XO[k].stream && !memcmp(p + HEADER, XO[k].id, 8)) { o = &XO[k]; break; }
        if (!o || (size != CHUNK_DIRECT && size != CHUNK_RELAY) || !count) return;
        if (count > MAX_CHUNKS_PER_REQ) count = MAX_CHUNKS_PER_REQ;
        if (XQn == JKX_HOST_QUEUE)
            memmove(XQ, XQ + 1, sizeof(XQ[0]) * (size_t)--XQn);   // the oldest is asked for again if it matters
        XQ[XQn].to = from;
        XQ[XQn].kind = k;
        XQ[XQn].direct = direct;
        XQ[XQn].next = first;
        XQ[XQn].end = first + count < first ? 0xFFFFFFFFu : first + count;
        XQ[XQn].size = size;
        XQ[XQn].block = 0;
        XQn++;
        jkx_HostPump();
    }
    else if (p[12] == T_BLOCK_REQ && n >= HEADER + 24)
    {
        // Added: blocks (maps.js since 2026-10-29), each queued for its parts
        jkx_offer* o = NULL;
        int k;
        uint32_t first = rd32(p + HEADER + 8), count = rd32(p + HEADER + 12), part = rd32(p + HEADER + 16);
        int accept = p[HEADER + 20];
        for (k = 0; k < 2; k++)
            if (XO[k].stream && !memcmp(p + HEADER, XO[k].id, 8)) { o = &XO[k]; break; }
        if (!o || (part != PART_DIRECT && part != PART_RELAY) || !count) return;
        if (count > MAX_BLOCKS_PER_REQ) count = MAX_BLOCKS_PER_REQ;
        if (!direct) part = PART_RELAY;   // the lobby closes sockets that send it more than 64 KiB
        for (uint32_t b = first; b < first + count && b < o->numBlocks; b++)
        {
            if (XQn == JKX_HOST_QUEUE)
                memmove(XQ, XQ + 1, sizeof(XQ[0]) * (size_t)--XQn);
            memset(&XQ[XQn], 0, sizeof(XQ[XQn]));
            XQ[XQn].to = from;
            XQ[XQn].kind = k;
            XQ[XQn].direct = direct;
            XQ[XQn].block = 1;
            XQ[XQn].b = b;
            XQ[XQn].size = part;
            XQ[XQn].accept = accept;
            XQn++;
        }
        jkx_HostPump();
    }
}

const char* jkx_Status(void) { return X.status; }
const char* jkx_Reason(void) { return X.reason; }
const char* jkx_Message(void) { return X.message; }
const char* jkx_ResultPath(void) { return X.resultPath; }

#endif
