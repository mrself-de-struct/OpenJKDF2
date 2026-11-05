// Mod fingerprints, computed exactly like the jkdf2mp browser build
// (packaging/wasm/mods.js in github.com/tristamus/OpenJKDF2), so desktop and
// browser players can tell whether they run the same mod.
//
// A mod is a folder the engine loads ahead of the game's files (-path, or a
// folder under expansions/ picked in Expansions & Mods). Its fingerprint is
// the SHA-256 of the folder packed into one GOB in a fixed way:
//   - every file, recursively (at most 8 folders deep), except names that
//     differ between copies without mattering (dot files, __MACOSX,
//     Thumbs.db, desktop.ini, .exe/.dll/.txt/.zip/... extensions, see
//     jkmod_Packable) and names a GOB entry can't hold (jkmod_ValidRel)
//   - sorted by relative path ('/'-separated, plain byte order)
//   - packed as "GOB ", version 20, table offset, the files back to back,
//     then the table: count, and per file offset, size, name[128].
// The pack is hashed while it's streamed, never built in memory.
//
// The lobby tail that names a host's mod, hex-encoded after its entry
// (version 2, mods.js since tristamus/OpenJKDF2 ffd26b53):
//   "JKMD" <version 2> <flags> <name length>
//   [<mod fingerprint: 32 bytes>] [<pack fingerprint: 16 bytes>] <name>
// flags: 1 the host runs a mod, named by <name>; 2 it runs the MotS
// Remastered pack (JKMRemasteredAssets.goo), whose fingerprint is the first
// 16 bytes of the file's SHA-256. A host with neither sends no tail.
// Version 1 (no flags, no pack) was "JKMD" 1 <name length> <fp> <name>.

#if defined(PLATFORM_DPLAY)

#include "jkmod.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define JKMOD_MAX_DEPTH   8
#define JKMOD_MAX_PATH    127
#define JKMOD_MAX_FILES   50000
#define JKMOD_GOB_ENTRY   136

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------

typedef struct
{
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    size_t   used;
} jkmod_sha;

static const uint32_t jkmod_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void jkmod_ShaBlock(jkmod_sha* s, const uint8_t* p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++)
    {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++)
    {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + jkmod_k[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void jkmod_ShaInit(jkmod_sha* s)
{
    static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->used = 0;
}

static void jkmod_ShaUpdate(jkmod_sha* s, const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    s->len += n;
    while (n)
    {
        size_t take = 64 - s->used;
        if (take > n) take = n;
        memcpy(s->buf + s->used, p, take);
        s->used += take; p += take; n -= take;
        if (s->used == 64) { jkmod_ShaBlock(s, s->buf); s->used = 0; }
    }
}

static void jkmod_ShaFinal(jkmod_sha* s, uint8_t out[32])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0, lenb[8];
    jkmod_ShaUpdate(s, &pad, 1);
    while (s->used != 56) jkmod_ShaUpdate(s, &zero, 1);
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    jkmod_ShaUpdate(s, lenb, 8);
    for (int i = 0; i < 8; i++) { out[4*i] = (uint8_t)(s->h[i] >> 24); out[4*i+1] = (uint8_t)(s->h[i] >> 16); out[4*i+2] = (uint8_t)(s->h[i] >> 8); out[4*i+3] = (uint8_t)s->h[i]; }
}

void jkmod_Sha256(const void* data, size_t n, uint8_t out[32])
{
    jkmod_sha s;
    jkmod_ShaInit(&s);
    jkmod_ShaUpdate(&s, data, n);
    jkmod_ShaFinal(&s, out);
}

int jkmod_Sha256FileRange(const char* path, uint64_t offset, uint64_t size, uint8_t out[32])
{
    static uint8_t buf[1 << 16];
    FILE* f = fopen(path, "rb");
    jkmod_sha s;
    if (!f) return 0;
#ifdef _WIN32
    if (_fseeki64(f, (__int64)offset, SEEK_SET) != 0) { fclose(f); return 0; }
#else
    if (fseeko(f, (off_t)offset, SEEK_SET) != 0) { fclose(f); return 0; }
#endif
    jkmod_ShaInit(&s);
    while (size)
    {
        size_t want = size < sizeof(buf) ? (size_t)size : sizeof(buf);
        size_t got = fread(buf, 1, want, f);
        if (got != want) { fclose(f); return 0; }
        jkmod_ShaUpdate(&s, buf, got);
        size -= got;
    }
    fclose(f);
    jkmod_ShaFinal(&s, out);
    return 1;
}

// ---------------------------------------------------------------------------
// Which files are in the pack (same rules as mods.js)
// ---------------------------------------------------------------------------

static int jkmod_EqNoCase(const char* a, const char* b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return !*a && !*b;
}

// packable(): every path component, and the file's own name/extension.
static int jkmod_Packable(const char* rel)
{
    static const char* exts[] = { "exe","dll","ico","ini","txt","url","pdf","htm","html","zip","rar","7z","bak","tmp" };
    const char* p = rel;
    const char* base = rel;
    const char* dot;
    while (1)
    {
        const char* slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len && p[0] == '.') return 0;
        if (len == 8 && !strncmp(p, "__macosx", 8)) return 0;
        if (len == 8)
        {
            char tmp[9];
            memcpy(tmp, p, 8); tmp[8] = 0;
            if (jkmod_EqNoCase(tmp, "__macosx")) return 0;
        }
        if (!slash) { base = p; break; }
        p = slash + 1;
    }
    if (jkmod_EqNoCase(base, "thumbs.db") || jkmod_EqNoCase(base, "desktop.ini")) return 0;
    dot = strrchr(base, '.');
    if (dot)
        for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
            if (jkmod_EqNoCase(dot + 1, exts[i])) return 0;
    return 1;
}

// validRel(): what a GOB entry name may be.
int jkmod_ValidRel(const char* rel)
{
    size_t len = strlen(rel);
    int parts = 1;
    const char* p = rel;
    if (!len || len > JKMOD_MAX_PATH) return 0;
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)rel[i];
        if (c < 0x20 || c > 0x7e) return 0;
        if (c == '/') parts++;
    }
    if (parts > JKMOD_MAX_DEPTH + 1) return 0;
    while (1)
    {
        const char* slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (!n) return 0;
        if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return 0;
        for (size_t i = 0; i < n; i++)
            if (strchr("\\:*?\"<>|", p[i])) return 0;
        if (p[n - 1] == ' ' || p[n - 1] == '.') return 0;
        if (!slash) break;
        p = slash + 1;
    }
    return 1;
}

typedef struct { char rel[JKMOD_MAX_PATH + 1]; char* path; uint64_t size; } jkmod_file;

typedef struct
{
    jkmod_file* files;
    int n, cap;
    int overflow;
} jkmod_list;

static void jkmod_Add(jkmod_list* l, const char* rel, const char* path, uint64_t size)
{
    if (l->n >= JKMOD_MAX_FILES) { l->overflow = 1; return; }
    if (l->n == l->cap)
    {
        int nc = l->cap ? l->cap * 2 : 256;
        jkmod_file* nf = (jkmod_file*)realloc(l->files, sizeof(jkmod_file) * (size_t)nc);
        if (!nf) { l->overflow = 1; return; }
        l->files = nf;
        l->cap = nc;
    }
    snprintf(l->files[l->n].rel, sizeof(l->files[l->n].rel), "%s", rel);
    l->files[l->n].path = strdup(path);
    l->files[l->n].size = size;
    l->n++;
}

static void jkmod_Walk(jkmod_list* l, const char* path, const char* rel, int depth)
{
#ifdef _WIN32
    char pattern[1024];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        const char* name = fd.cFileName;
        char p[1024], r[1024];
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        snprintf(p, sizeof(p), "%s\\%s", path, name);
        snprintf(r, sizeof(r), rel[0] ? "%s/%s" : "%s%s", rel, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (depth < JKMOD_MAX_DEPTH && jkmod_Packable(r)) jkmod_Walk(l, p, r, depth + 1);
        }
        else if (jkmod_Packable(r) && jkmod_ValidRel(r))
            jkmod_Add(l, r, p, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(path);
    struct dirent* e;
    if (!d) return;
    while ((e = readdir(d)))
    {
        char p[1024], r[1024];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        snprintf(r, sizeof(r), rel[0] ? "%s/%s" : "%s%s", rel, e->d_name);
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
        {
            if (depth < JKMOD_MAX_DEPTH && jkmod_Packable(r)) jkmod_Walk(l, p, r, depth + 1);
        }
        else if (S_ISREG(st.st_mode) && jkmod_Packable(r) && jkmod_ValidRel(r))
            jkmod_Add(l, r, p, (uint64_t)st.st_size);
    }
    closedir(d);
#endif
}

static int jkmod_CmpRel(const void* a, const void* b)
{
    return strcmp(((const jkmod_file*)a)->rel, ((const jkmod_file*)b)->rel);
}

static void jkmod_FreeList(jkmod_list* l)
{
    for (int i = 0; i < l->n; i++) free(l->files[i].path);
    free(l->files);
    memset(l, 0, sizeof(*l));
}

static void jkmod_Put32(jkmod_sha* s, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    jkmod_ShaUpdate(s, b, 4);
}

// ---------------------------------------------------------------------------

const char* jkmod_BaseName(const char* path);

// Hashes the pack of an already-built, sorted list; frees the list.
static int jkmod_HashList(jkmod_list* lp, uint8_t out[32])
{
    jkmod_list l = *lp;
    jkmod_sha s;
    uint64_t data = 0, at = 12;
    uint8_t* buf;
    int ok = 1;

    for (int i = 0; i < l.n; i++) data += l.files[i].size;
    if (12 + data > 0xFFFFFFFFull) { jkmod_FreeList(&l); return 0; }   // a GOB's offsets are 32-bit

    jkmod_ShaInit(&s);
    jkmod_ShaUpdate(&s, "GOB ", 4);
    jkmod_Put32(&s, 20);
    jkmod_Put32(&s, (uint32_t)(12 + data));

    buf = (uint8_t*)malloc(1 << 16);
    if (!buf) { jkmod_FreeList(&l); return 0; }
    for (int i = 0; i < l.n && ok; i++)
    {
        FILE* f = fopen(l.files[i].path, "rb");
        uint64_t left = l.files[i].size;
        if (!f) { ok = 0; break; }
        while (left)
        {
            size_t want = left > (1 << 16) ? (1 << 16) : (size_t)left;
            size_t got = fread(buf, 1, want, f);
            if (got != want) { ok = 0; break; }    // file changed while reading
            jkmod_ShaUpdate(&s, buf, got);
            left -= got;
        }
        fclose(f);
    }
    free(buf);

    if (ok)
    {
        jkmod_Put32(&s, (uint32_t)l.n);
        for (int i = 0; i < l.n; i++)
        {
            uint8_t name[128];
            jkmod_Put32(&s, (uint32_t)at);
            jkmod_Put32(&s, (uint32_t)l.files[i].size);
            memset(name, 0, sizeof(name));
            memcpy(name, l.files[i].rel, strlen(l.files[i].rel));
            jkmod_ShaUpdate(&s, name, sizeof(name));
            at += l.files[i].size;
        }
        jkmod_ShaFinal(&s, out);
    }
    jkmod_FreeList(&l);
    return ok;
}

int jkmod_Fingerprint(const char* dir, uint8_t out[32])
{
    jkmod_list l;
    memset(&l, 0, sizeof(l));
    jkmod_Walk(&l, dir, "", 0);
    if (l.overflow) { jkmod_FreeList(&l); return 0; }
    qsort(l.files, (size_t)l.n, sizeof(jkmod_file), jkmod_CmpRel);
    return jkmod_HashList(&l, out);
}

// A one-file mod: the browser build counts each non-original GOB in
// resource/ and mods/ (which the engine always loads) as a mod of its own.
int jkmod_FingerprintFile(const char* path, uint8_t out[32])
{
    jkmod_list l;
    const char* name = jkmod_BaseName(path);
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    uint64_t size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    uint64_t size = (uint64_t)st.st_size;
#endif
    memset(&l, 0, sizeof(l));
    if (!jkmod_ValidRel(name)) return 0;
    jkmod_Add(&l, name, path, size);
    if (l.overflow) { jkmod_FreeList(&l); return 0; }
    return jkmod_HashList(&l, out);
}

static void jkmod_CollectAlwaysLoaded(jkmod_list* l, int mots);

// The pack in memory, to send it (jkweb_xfer.c): the same files, order and
// layout the fingerprint hashes.
const char* jkmod_Pack(const char* path, int isFile, int maxFiles, uint64_t maxData,
                       uint8_t** outBytes, uint32_t* outLen, jkmod_packEntry** outEntries, int* outCount)
{
    jkmod_list l;
    uint64_t data = 0, at = 12, table;
    uint8_t* b;
    jkmod_packEntry* e;
    const char* fail = NULL;

    *outBytes = NULL; *outLen = 0; *outEntries = NULL; *outCount = 0;
    memset(&l, 0, sizeof(l));
    if (isFile == JKMOD_SRC_ALWAYS_JK || isFile == JKMOD_SRC_ALWAYS_MOTS)
    {
        jkmod_CollectAlwaysLoaded(&l, isFile == JKMOD_SRC_ALWAYS_MOTS);
        qsort(l.files, (size_t)l.n, sizeof(jkmod_file), jkmod_CmpRel);
    }
    else if (isFile)
    {
        const char* name = jkmod_BaseName(path);
#ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return "error";
        uint64_t size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
#else
        struct stat st;
        if (stat(path, &st) != 0) return "error";
        uint64_t size = (uint64_t)st.st_size;
#endif
        if (!jkmod_ValidRel(name)) return "error";
        jkmod_Add(&l, name, path, size);
    }
    else
    {
        jkmod_Walk(&l, path, "", 0);
        qsort(l.files, (size_t)l.n, sizeof(jkmod_file), jkmod_CmpRel);
    }
    if (l.overflow || l.n > maxFiles) { jkmod_FreeList(&l); return "too-many-files"; }
    for (int i = 0; i < l.n; i++) data += l.files[i].size;
    if (data > maxData) { jkmod_FreeList(&l); return "too-big"; }

    table = 12 + data;
    b = (uint8_t*)malloc((size_t)(table + 4 + (uint64_t)l.n * JKMOD_GOB_ENTRY));
    e = (jkmod_packEntry*)calloc((size_t)(l.n ? l.n : 1), sizeof(jkmod_packEntry));
    if (!b || !e) { free(b); free(e); jkmod_FreeList(&l); return "error"; }
    memcpy(b, "GOB ", 4);
#define JKMOD_W32(p, v) do { uint32_t v_ = (uint32_t)(v); (p)[0] = (uint8_t)v_; (p)[1] = (uint8_t)(v_ >> 8); (p)[2] = (uint8_t)(v_ >> 16); (p)[3] = (uint8_t)(v_ >> 24); } while (0)
    JKMOD_W32(b + 4, 20);
    JKMOD_W32(b + 8, table);
    JKMOD_W32(b + table, l.n);
    for (int i = 0; i < l.n && !fail; i++)
    {
        FILE* f = fopen(l.files[i].path, "rb");
        uint8_t* ent = b + table + 4 + (size_t)i * JKMOD_GOB_ENTRY;
        if (!f || fread(b + at, 1, (size_t)l.files[i].size, f) != (size_t)l.files[i].size) fail = "changed";
        if (f) { if (!fail && fgetc(f) != EOF) fail = "changed"; fclose(f); }
        JKMOD_W32(ent, at);
        JKMOD_W32(ent + 4, l.files[i].size);
        memset(ent + 8, 0, 128);
        memcpy(ent + 8, l.files[i].rel, strlen(l.files[i].rel));
        e[i].offset = (uint32_t)at;
        e[i].size = (uint32_t)l.files[i].size;
        snprintf(e[i].name, sizeof(e[i].name), "%s", l.files[i].rel);
        at += l.files[i].size;
    }
#undef JKMOD_W32
    if (fail) { free(b); free(e); jkmod_FreeList(&l); return fail; }
    *outBytes = b;
    *outLen = (uint32_t)(table + 4 + (uint64_t)l.n * JKMOD_GOB_ENTRY);
    *outEntries = e;
    *outCount = l.n;
    jkmod_FreeList(&l);
    return NULL;
}

// The non-original GOBs (.gob, or .goo for MotS) the engine always loads, from mods/
// then resource/ (folder names in any case), named by file name (rel), the first of
// a name kept (the browser's precedence). Not the retail GOBs, the MotS Remastered
// pack (part of the game to the browser), or the graphics packs the browser keeps as
// each player's own (index.html OPTIONAL_PACKS).
static int jkmod_IsAlwaysLoadedMod(const char* n, int mots)
{
    static const char* retailJk[] = { "res1hi.gob", "res1low.gob", "res2.gob", "res1demo.gob" };
    static const char* retailMots[] = { "jkmres.goo", "jkmsndlo.goo" };
    static const char* graphicsJk[] = { "jke.gob", "jkr.gob", "jk hd models.gob" };
    const char* ext = mots ? ".goo" : ".gob";
    const char** retail = mots ? retailMots : retailJk;
    int nRetail = mots ? 2 : 4;
    size_t nl = strlen(n);
    if (n[0] == '.' || nl < 4 || !jkmod_EqNoCase(n + nl - 4, ext)) return 0;
    for (int r = 0; r < nRetail; r++) if (jkmod_EqNoCase(n, retail[r])) return 0;
    if (mots && jkmod_EqNoCase(n, JKMOD_PACK_FILE)) return 0;
    if (!mots)
        for (int r = 0; r < 3; r++) if (jkmod_EqNoCase(n, graphicsJk[r])) return 0;
    return 1;
}

static void jkmod_AddAlwaysLoaded(jkmod_list* l, const char* n, const char* path, uint64_t size)
{
    for (int i = 0; i < l->n; i++)
        if (jkmod_EqNoCase(l->files[i].rel, n)) return;
    if (jkmod_ValidRel(n)) jkmod_Add(l, n, path, size);
}

static void jkmod_CollectAlwaysLoaded(jkmod_list* l, int mots)
{
    static const char* dirsWanted[] = { "mods", "resource" };   // the browser's precedence order
    for (int w = 0; w < 2; w++)
    {
#ifdef _WIN32
        // Windows paths aren't case-sensitive.
        char pattern[64];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pattern, sizeof(pattern), "%s\\*", dirsWanted[w]);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do
        {
            char p[512];
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!jkmod_IsAlwaysLoadedMod(fd.cFileName, mots)) continue;
            snprintf(p, sizeof(p), "%s\\%s", dirsWanted[w], fd.cFileName);
            jkmod_AddAlwaysLoaded(l, fd.cFileName, p, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
#else
        DIR* top = opendir(".");
        struct dirent* te;
        if (!top) return;
        while ((te = readdir(top)))
        {
            DIR* d;
            struct dirent* e;
            if (!jkmod_EqNoCase(te->d_name, dirsWanted[w])) continue;
            d = opendir(te->d_name);
            if (!d) continue;
            while ((e = readdir(d)))
            {
                char p[512];
                struct stat st;
                if (!jkmod_IsAlwaysLoadedMod(e->d_name, mots)) continue;
                snprintf(p, sizeof(p), "%s/%s", te->d_name, e->d_name);
                if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
                jkmod_AddAlwaysLoaded(l, e->d_name, p, (uint64_t)st.st_size);
            }
            closedir(d);
        }
        closedir(top);
#endif
    }
}

// Returns how many; the first one's path is copied out.
int jkmod_FindAlwaysLoaded(int mots, char* firstPath, size_t len)
{
    jkmod_list l;
    int n;
    memset(&l, 0, sizeof(l));
    if (len) firstPath[0] = 0;
    jkmod_CollectAlwaysLoaded(&l, mots);
    n = l.n;
    if (n && len) snprintf(firstPath, len, "%s", l.files[0].path);
    jkmod_FreeList(&l);
    return n;
}

// Added: the i-th of them, sorted by file name as the pack orders them (paths for noshare.txt,
// names for the mod's name). 0 past the end.
int jkmod_GetAlwaysLoaded(int mots, int i, char* path, size_t len)
{
    jkmod_list l;
    int ok = 0;
    memset(&l, 0, sizeof(l));
    jkmod_CollectAlwaysLoaded(&l, mots);
    qsort(l.files, (size_t)l.n, sizeof(jkmod_file), jkmod_CmpRel);
    if (i >= 0 && i < l.n) { snprintf(path, len, "%s", l.files[i].path); ok = 1; }
    jkmod_FreeList(&l);
    return ok;
}

// Several of them make one mod, packed as a folder holding them would be: the browser
// build puts the mods a player ticks together in one folder, and a joiner who got that
// from a host has it in netmods/<fp16> with the same file names, so the fingerprints match.
int jkmod_FingerprintAlwaysLoaded(int mots, uint8_t out[32])
{
    jkmod_list l;
    memset(&l, 0, sizeof(l));
    jkmod_CollectAlwaysLoaded(&l, mots);
    if (l.overflow || !l.n) { jkmod_FreeList(&l); return 0; }
    qsort(l.files, (size_t)l.n, sizeof(jkmod_file), jkmod_CmpRel);
    return jkmod_HashList(&l, out);
}

// ---------------------------------------------------------------------------
// Tails
// ---------------------------------------------------------------------------

static const char jkmod_hex[] = "0123456789abcdef";

static void jkmod_CleanName(const char* name, char clean[JKMOD_MAX_NAME + 1])
{
    // cleanName(): printable ASCII ('?' for anything else), trimmed, "mod" if
    // empty, then cut to 63 (in that order, so a cut can end in a space).
    size_t start = 0, end = name ? strlen(name) : 0, n = 0;
    while (start < end && name[start] == ' ') start++;
    while (end > start && name[end - 1] == ' ') end--;
    for (size_t i = start; i < end && n < JKMOD_MAX_NAME; i++)
    {
        unsigned char c = (unsigned char)name[i];
        // One '?' per UTF-8 character, as JS counts them ("caf\xc3\xa9" -> "caf?").
        if (c >= 0x80 && c <= 0xbf && i > start && (unsigned char)name[i - 1] >= 0x80) continue;
        clean[n++] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?';
    }
    clean[n] = 0;
    if (!n) strcpy(clean, "mod");
}

void jkmod_MakeTail(const uint8_t* fp, const char* name, const uint8_t* packFp, char* outHex, size_t outLen)
{
    uint8_t b[7 + 32 + JKMOD_PACK_FP + JKMOD_MAX_NAME];
    char clean[JKMOD_MAX_NAME + 1] = "";
    size_t at = 7, nameLen;

    if (outLen) outHex[0] = 0;
    if (!fp && !packFp) return;                          // no tail
    if (fp) jkmod_CleanName(name, clean);
    nameLen = strlen(clean);

    memcpy(b, "JKMD", 4);
    b[4] = 2;
    b[5] = (uint8_t)((fp ? JKMOD_TAIL_MOD : 0) | (packFp ? JKMOD_TAIL_PACK : 0));
    b[6] = (uint8_t)nameLen;
    if (fp) { memcpy(b + at, fp, 32); at += 32; }
    if (packFp) { memcpy(b + at, packFp, JKMOD_PACK_FP); at += JKMOD_PACK_FP; }
    memcpy(b + at, clean, nameLen);
    at += nameLen;

    if (outLen < 2 * at + 1) return;
    for (size_t i = 0; i < at; i++) { outHex[2*i] = jkmod_hex[b[i] >> 4]; outHex[2*i+1] = jkmod_hex[b[i] & 15]; }
    outHex[2 * at] = 0;
}

static int jkmod_Nib(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int jkmod_ParseTail(const char* hex, uint8_t fp[32], char* name, size_t nameLen, uint8_t packFp[JKMOD_PACK_FP])
{
    uint8_t b[7 + 32 + JKMOD_PACK_FP + JKMOD_MAX_NAME];
    size_t len = hex ? strlen(hex) : 0, n, at, nl;
    int flags;
    if (!len) return 0;                                  // host runs nothing
    if (len % 2 || len / 2 < 7 || len / 2 > sizeof(b)) return -1;
    n = len / 2;
    for (size_t i = 0; i < n; i++)
    {
        int hi = jkmod_Nib(hex[2*i]), lo = jkmod_Nib(hex[2*i+1]);
        if (hi < 0 || lo < 0) return -1;
        b[i] = (uint8_t)(hi << 4 | lo);
    }
    if (memcmp(b, "JKMD", 4)) return -1;
    if (b[4] == 1)
    {
        // Version 1: <name length> <fp> <name>, always a mod.
        if (b[5] > JKMOD_MAX_NAME || n != 38u + b[5]) return -1;
        flags = JKMOD_TAIL_MOD;
        nl = b[5];
        at = 6;
    }
    else if (b[4] == 2)
    {
        // Same checks as mods.js parseTail().
        flags = b[5];
        nl = b[6];
        if (!flags || (flags & ~(JKMOD_TAIL_MOD | JKMOD_TAIL_PACK)) || nl > JKMOD_MAX_NAME
            || !(flags & JKMOD_TAIL_MOD) != !nl
            || n != 7 + ((flags & JKMOD_TAIL_MOD) ? 32u : 0u) + ((flags & JKMOD_TAIL_PACK) ? (size_t)JKMOD_PACK_FP : 0u) + nl)
            return -1;
        at = 7;
    }
    else
        return -1;

    if (flags & JKMOD_TAIL_MOD) { memcpy(fp, b + at, 32); at += 32; }
    if (flags & JKMOD_TAIL_PACK) { memcpy(packFp, b + at, JKMOD_PACK_FP); at += JKMOD_PACK_FP; }
    if (nameLen)
    {
        char raw[JKMOD_MAX_NAME + 1];
        memcpy(raw, b + at, nl);
        raw[nl] = 0;
        if (flags & JKMOD_TAIL_MOD)
        {
            char clean[JKMOD_MAX_NAME + 1];
            jkmod_CleanName(raw, clean);
            snprintf(name, nameLen, "%s", clean);
        }
        else
            name[0] = 0;
    }
    return flags;
}

// ---------------------------------------------------------------------------
// The MotS Remastered pack
// ---------------------------------------------------------------------------

int jkmod_FindRemasterPack(char* outPath, size_t len)
{
    static const char* dirs[] = { "mods", "resource" };   // mods.js PACK_DIRS
    for (int w = 0; w < 2; w++)
    {
#ifdef _WIN32
        char p[64];
        DWORD a;
        snprintf(p, sizeof(p), "%s\\JKMRemasteredAssets.goo", dirs[w]);   // case doesn't matter here
        a = GetFileAttributesA(p);
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
        {
            snprintf(outPath, len, "%s", p);
            return 1;
        }
#else
        DIR* top = opendir(".");
        struct dirent* te;
        if (!top) return 0;
        while ((te = readdir(top)))
        {
            DIR* d;
            struct dirent* e;
            if (!jkmod_EqNoCase(te->d_name, dirs[w])) continue;
            d = opendir(te->d_name);
            if (!d) continue;
            while ((e = readdir(d)))
            {
                char p[512];
                struct stat st;
                if (!jkmod_EqNoCase(e->d_name, JKMOD_PACK_FILE)) continue;
                snprintf(p, sizeof(p), "%s/%s", te->d_name, e->d_name);
                if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
                snprintf(outPath, len, "%s", p);
                closedir(d);
                closedir(top);
                return 1;
            }
            closedir(d);
        }
        closedir(top);
#endif
    }
    return 0;
}

int jkmod_PackFingerprintCached(const char* path, uint8_t out[JKMOD_PACK_FP])
{
    // One pack per game folder, ~209 MB: hash it once per path.
    static char cachedPath[512];
    static uint8_t cachedFp[JKMOD_PACK_FP];
    static int cachedState;   // 1 ok, -1 failed
    if (cachedState && !strcmp(cachedPath, path))
    {
        if (cachedState < 0) return 0;
        memcpy(out, cachedFp, JKMOD_PACK_FP);
        return 1;
    }
    {
        FILE* f = fopen(path, "rb");
        jkmod_sha s;
        uint8_t full[32];
        static uint8_t buf[1 << 16];
        size_t got;
        snprintf(cachedPath, sizeof(cachedPath), "%s", path);
        cachedState = -1;
        if (!f) return 0;
        jkmod_ShaInit(&s);
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
            jkmod_ShaUpdate(&s, buf, got);
        if (ferror(f)) { fclose(f); return 0; }
        fclose(f);
        jkmod_ShaFinal(&s, full);
        memcpy(cachedFp, full, JKMOD_PACK_FP);
        cachedState = 1;
        memcpy(out, cachedFp, JKMOD_PACK_FP);
        return 1;
    }
}

// ---------------------------------------------------------------------------
// Finding a local copy
// ---------------------------------------------------------------------------

typedef struct { char path[512]; uint8_t fp[32]; int state; } jkmod_cacheEntry;   // state: 1 ok, -1 failed
static jkmod_cacheEntry jkmod_cache[64];

int jkmod_FingerprintCached(const char* dir, uint8_t out[32])
{
    int freeSlot = -1;
    for (int i = 0; i < 64; i++)
    {
        if (jkmod_cache[i].state && !strcmp(jkmod_cache[i].path, dir))
        {
            if (jkmod_cache[i].state < 0) return 0;
            memcpy(out, jkmod_cache[i].fp, 32);
            return 1;
        }
        if (!jkmod_cache[i].state && freeSlot < 0) freeSlot = i;
    }
    {
        int ok = jkmod_Fingerprint(dir, out);
        if (freeSlot >= 0)
        {
            snprintf(jkmod_cache[freeSlot].path, sizeof(jkmod_cache[freeSlot].path), "%s", dir);
            if (ok) memcpy(jkmod_cache[freeSlot].fp, out, 32);
            jkmod_cache[freeSlot].state = ok ? 1 : -1;
        }
        return ok;
    }
}

int jkmod_FingerprintFileCached(const char* path, uint8_t out[32])
{
    // Same cache, keyed so a file and a folder of the same name don't collide.
    char key[sizeof(jkmod_cache[0].path)];
    int freeSlot = -1;
    if ((size_t)snprintf(key, sizeof(key), "file:%s", path) >= sizeof(key))
        return jkmod_FingerprintFile(path, out); // too long to cache
    for (int i = 0; i < 64; i++)
    {
        if (jkmod_cache[i].state && !strcmp(jkmod_cache[i].path, key))
        {
            if (jkmod_cache[i].state < 0) return 0;
            memcpy(out, jkmod_cache[i].fp, 32);
            return 1;
        }
        if (!jkmod_cache[i].state && freeSlot < 0) freeSlot = i;
    }
    {
        int ok = jkmod_FingerprintFile(path, out);
        if (freeSlot >= 0)
        {
            snprintf(jkmod_cache[freeSlot].path, sizeof(jkmod_cache[freeSlot].path), "%s", key);
            if (ok) memcpy(jkmod_cache[freeSlot].fp, out, 32);
            jkmod_cache[freeSlot].state = ok ? 1 : -1;
        }
        return ok;
    }
}

int jkmod_FindLocal(const uint8_t fp[32], char* outPath, size_t outLen)
{
    static const char* roots[] = { "expansions", "mods", "netmods" };   // netmods: downloaded (jkweb_xfer.c)
    for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); r++)
    {
#ifdef _WIN32
        char pattern[512];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pattern, sizeof(pattern), "%s\\*", roots[r]);
        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do
        {
            char path[512];
            uint8_t got[32];
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            snprintf(path, sizeof(path), "%s\\%s", roots[r], fd.cFileName);
            if (jkmod_FingerprintCached(path, got) && !memcmp(got, fp, 32))
            {
                snprintf(outPath, outLen, "%s", path);
                FindClose(h);
                return 1;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
#else
        DIR* d = opendir(roots[r]);
        struct dirent* e;
        if (!d) continue;
        while ((e = readdir(d)))
        {
            char path[512];
            uint8_t got[32];
            struct stat st;
            if (e->d_name[0] == '.') continue;
            snprintf(path, sizeof(path), "%s/%s", roots[r], e->d_name);
            if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            if (jkmod_FingerprintCached(path, got) && !memcmp(got, fp, 32))
            {
                snprintf(outPath, outLen, "%s", path);
                closedir(d);
                return 1;
            }
        }
        closedir(d);
#endif
    }
    return 0;
}

const char* jkmod_BaseName(const char* path)
{
    const char* a = strrchr(path, '/');
    const char* b = strrchr(path, '\\');
    const char* p = a > b ? a : b;
    return p ? p + 1 : path;
}

#endif // PLATFORM_DPLAY
