// Tests jkweb_xfer.c (map and mod downloads, joiner side) against a host
// simulated the way packaging/wasm/maps.js serves (planShareRanges, INFO,
// CHUNK), in a scratch game folder with a made-up retail GOB.
//
// cc -std=gnu11 -DPLATFORM_DPLAY -DJKX_TESTS -Isrc/Platform/Networking/DPlay src/Tests/jkweb_xfer_test.c src/Platform/Networking/DPlay/jkweb_xfer.c src/Platform/Networking/DPlay/jkmod.c -lm && ./a.out /tmp/some-empty-dir

#include "jkweb_xfer.h"
#include "jkmod.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// ---- fake clock and transport

static uint64_t now = 1000;
uint64_t jkx_TestNowMs(void) { return now; }

static int directUp = 0;
int jkweb_XferDirectUp(void) { return directUp; }
int jkweb_XferDirectPossible(void) { return 0; }
int jkweb_XferHostState(void) { return 1; }
// Loopback host: a link that takes only so many frames per pump.
static int canSendBudget = 1000000;
int jkweb_XferCanSend(uint32_t to, int allowDirect) { (void)to; (void)allowDirect; return canSendBudget-- > 0; }
static int noteLosses = 0;
void jkweb_XferNoteLoss(uint32_t to) { (void)to; noteLosses++; }
uint32_t jkweb_XferPartMax(void) { return 0; }

typedef struct frame { struct frame* next; size_t n; uint8_t d[]; } frame;
static frame *qHead, *qTail;

static void deliver(const uint8_t* d, size_t n)
{
    frame* f = malloc(sizeof(frame) + n);
    f->next = NULL; f->n = n; memcpy(f->d, d, n);
    if (qTail) qTail->next = f; else qHead = f;
    qTail = f;
}

static int loopback;

void jkweb_XferPump(void)
{
    if (loopback) { canSendBudget = 2; jkx_HostPump(); }
    while (qHead)
    {
        frame* f = qHead;
        qHead = f->next;
        if (!qHead) qTail = NULL;
        jkx_OnFrame(rd32(f->d), f->d, f->n, directUp ? JKX_VIA_DIRECT : JKX_VIA_RELAY);
        free(f);
    }
}

// ---- the host

static struct
{
    int answer;              // 0: never answers INFO
    int kind;
    char info[512];
    uint8_t* stream;
    uint32_t streamLen;
    uint8_t id[8];
    int dropEvery;           // drop each Nth chunk the first time it's asked for
    uint8_t* sentOnce;
    int infoReqs, chunkReqs, chunksSent;
} H;

static void host_frame(uint8_t* f, int type, int kind)
{
    memset(f, 0, 16);
    wr32(f, 1); wr32(f + 4, 2);
    memcpy(f + 8, "JKMT", 4);
    f[12] = (uint8_t)type; f[13] = 1; f[14] = (uint8_t)kind;
}

// Loopback: our own host side (jkx_HostOffer) answers our joiner side.
static int hostChunkFrames = 0;   // chunks or block parts our host side sent
static int hostBlockFrames = 0;   // block parts

int jkweb_XferSend(uint32_t to, const uint8_t* p, size_t n, int allowDirect)
{
    (void)allowDirect;
    if (loopback)
    {
        if (to == 1)
        {
            uint8_t* copy = malloc(n);
            memcpy(copy, p, n);
            jkx_OnFrame(2, copy, n, directUp ? JKX_VIA_DIRECT : JKX_VIA_RELAY);   // the lobby stamps player 2
            free(copy);
        }
        else
        {
            CHECK(to == 2 && rd32(p) == 1 && rd32(p + 4) == 2 && !memcmp(p + 8, "JKMT", 4));
            if (p[12] == 4 || p[12] == 6) hostChunkFrames++;
            if (p[12] == 6) hostBlockFrames++;
            deliver(p, n);
        }
        return directUp ? JKX_VIA_DIRECT : JKX_VIA_RELAY;
    }
    CHECK(to == 1 && n >= 16 && !memcmp(p + 8, "JKMT", 4) && rd32(p + 4) == 1 && p[13] == 1);
    if (p[12] == 1)
    {
        H.infoReqs++;
        CHECK(n == 16 && p[14] == H.kind);
        if (H.answer)
        {
            uint8_t f[600];
            size_t l = strlen(H.info);
            host_frame(f, 2, p[14]);
            memcpy(f + 16, H.info, l);
            deliver(f, 16 + l);
        }
    }
    else if (p[12] == 3)
    {
        uint32_t size = rd32(p + 24), first = rd32(p + 28), count = rd32(p + 32);
        H.chunkReqs++;
        CHECK(n == 36 && !memcmp(p + 16, H.id, 8));
        CHECK(size == 16384 || size == 49152);
        CHECK(count <= 128);
        for (uint32_t i = first; i < first + count && (uint64_t)i * size < H.streamLen; i++)
        {
            uint32_t len = H.streamLen - i * size;
            uint8_t* f;
            if (len > size) len = size;
            if (H.dropEvery && i % H.dropEvery == 3 && !H.sentOnce[i]) { H.sentOnce[i] = 1; continue; }
            f = malloc(28 + len);
            host_frame(f, 4, p[14]);
            memcpy(f + 16, H.id, 8);
            wr32(f + 24, i);
            memcpy(f + 28, H.stream + (size_t)i * size, len);
            deliver(f, 28 + len);
            free(f);
            H.chunksSent++;
        }
    }
    return directUp ? JKX_VIA_DIRECT : JKX_VIA_RELAY;
}

// ---- GOBs

typedef struct { const char* name; const uint8_t* data; uint32_t size; } gobfile;

static uint8_t* make_gob(const gobfile* files, int n, uint32_t* outLen, uint32_t* offs)
{
    uint32_t data = 0, table, total;
    uint8_t* g;
    for (int i = 0; i < n; i++) data += files[i].size;
    table = 12 + data;
    total = table + 4 + n * 136;
    g = calloc(1, total);
    memcpy(g, "GOB ", 4); wr32(g + 4, 20); wr32(g + 8, table); wr32(g + table, (uint32_t)n);
    for (int i = 0, at = 12; i < n; i++)
    {
        memcpy(g + at, files[i].data, files[i].size);
        wr32(g + table + 4 + i * 136, (uint32_t)at);
        wr32(g + table + 8 + i * 136, files[i].size);
        strcpy((char*)g + table + 12 + i * 136, files[i].name);
        if (offs) offs[i] = (uint32_t)at;
        at += files[i].size;
    }
    *outLen = total;
    return g;
}

static void write_file(const char* path, const uint8_t* d, size_t n)
{
    FILE* f = fopen(path, "wb");
    fwrite(d, 1, n, f);
    fclose(f);
}

static uint8_t* read_file(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    uint8_t* d;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    d = malloc(*n ? *n : 1);
    if (fread(d, 1, *n, f) != *n) { fclose(f); free(d); return NULL; }
    fclose(f);
    return d;
}

static void fill(uint8_t* p, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; p[i] = (uint8_t)(seed >> 16); }
}

// planShareRanges(): ranges that are copies of retail files become references.
typedef struct { uint32_t off, size; } range;

static void host_offer(int kind, const char* name, const char* ext, const uint8_t* file, uint32_t len,
                       const range* refs, int numRefs)
{
    uint8_t sha[32], rsha[32];
    char hex[65];
    uint32_t refsBytes = 4 + numRefs * 40, inline_ = 0, pos = 0, at;
    for (int i = 0; i < numRefs; i++) { if (refs[i].off > pos) inline_ += refs[i].off - pos; pos = refs[i].off + refs[i].size; }
    inline_ += len - pos;
    free(H.stream);
    free(H.sentOnce);
    H.streamLen = refsBytes + inline_;
    H.stream = malloc(H.streamLen);
    wr32(H.stream, (uint32_t)numRefs);
    for (int i = 0; i < numRefs; i++)
    {
        wr32(H.stream + 4 + i * 40, refs[i].off);
        wr32(H.stream + 8 + i * 40, refs[i].size);
        jkmod_Sha256(file + refs[i].off, refs[i].size, rsha);
        memcpy(H.stream + 12 + i * 40, rsha, 32);
    }
    at = refsBytes; pos = 0;
    for (int i = 0; i < numRefs; i++)
    {
        if (refs[i].off > pos) { memcpy(H.stream + at, file + pos, refs[i].off - pos); at += refs[i].off - pos; }
        pos = refs[i].off + refs[i].size;
    }
    memcpy(H.stream + at, file + pos, len - pos);
    jkmod_Sha256(file, len, sha);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", sha[i]);
    memcpy(H.id, sha, 8);
    H.kind = kind;
    snprintf(H.info, sizeof(H.info),
             "{\"status\":\"ok\",\"name\":\"%s\",\"ext\":\"%s\",\"size\":%u,\"sha256\":\"%s\",\"streamSize\":%u,\"refsBytes\":%u}",
             name, ext, len, hex, H.streamLen, refsBytes);
    H.sentOnce = calloc(H.streamLen / 16384 + 2, 1);
    H.infoReqs = H.chunkReqs = H.chunksSent = 0;
}

static int run(void)
{
    int r = JKX_BUSY;
    for (int i = 0; i < 100000 && r == JKX_BUSY; i++)
    {
        r = jkx_Tick();
        now += 50;
    }
    return r;
}

int main(int argc, char** argv)
{
    uint8_t a[3000], b[70000], c[5000], whole[200];
    uint32_t retailLen, mapLen, offs[3];
    uint8_t* retail;
    uint8_t* map;
    size_t n;

    if (argc < 2 || chdir(argv[1]) != 0) { printf("usage: %s <empty scratch dir>\n", argv[0]); return 2; }
    mkdir("resource", 0755);
    mkdir("Episode", 0755);   // another case than the engine's spelling

    // The retail GOB: files A and C.
    fill(a, sizeof(a), 1); fill(c, sizeof(c), 3); fill(b, sizeof(b), 2); fill(whole, sizeof(whole), 4);
    {
        gobfile rf[] = { { "cog/a.cog", a, sizeof(a) }, { "mat/c.mat", c, sizeof(c) } };
        retail = make_gob(rf, 2, &retailLen, NULL);
        write_file("resource/Res2.gob", retail, retailLen);
    }

    // A custom map: a copy of A, a custom B, a copy of C.
    {
        gobfile mf[] = { { "cog/a.cog", a, sizeof(a) }, { "jkl/b.jkl", b, sizeof(b) }, { "mat/c.mat", c, sizeof(c) } };
        map = make_gob(mf, 3, &mapLen, offs);
    }
    range refs[] = { { offs[0], sizeof(a) }, { offs[2], sizeof(c) } };

    // 1. Map through the lobby (48 KiB chunks), rebuilt with the retail GOB's files.
    host_offer(JKX_KIND_MAP, "MyMap", "gob", map, mapLen, refs, 2);
    H.answer = 1;
    H.dropEvery = 0;
    jkx_StartMap("mymap", "gob");
    CHECK(run() == JKX_READY);
    {
        uint8_t* got = read_file("Episode/mymap.gob", &n);
        CHECK(got && n == mapLen && !memcmp(got, map, mapLen));
        free(got);
    }

    // 2. Same file already here: ready after INFO, nothing downloaded.
    H.chunkReqs = 0;
    jkx_StartMap("mymap", "gob");
    CHECK(run() == JKX_READY);
    CHECK(H.chunkReqs == 0);

    // 3. A different local copy: replaced, ours kept as .bak; direct link,
    //    16 KiB chunks, every 5th lost once.
    {
        uint8_t other[100];
        fill(other, sizeof(other), 9);
        write_file("Episode/mymap.gob", other, sizeof(other));
        b[100] ^= 1;
        free(map);
        gobfile mf[] = { { "cog/a.cog", a, sizeof(a) }, { "jkl/b.jkl", b, sizeof(b) }, { "mat/c.mat", c, sizeof(c) } };
        map = make_gob(mf, 3, &mapLen, offs);
        host_offer(JKX_KIND_MAP, "mymap", "gob", map, mapLen, refs, 2);
        H.dropEvery = 5;
        directUp = 1;
        jkx_StartMap("mymap", "gob");
        CHECK(run() == JKX_READY);
        uint8_t* got = read_file("Episode/mymap.gob", &n);
        CHECK(got && n == mapLen && !memcmp(got, map, mapLen));
        free(got);
        got = read_file("Episode/mymap.gob.bak", &n);
        CHECK(got && n == sizeof(other) && !memcmp(got, other, n));
        free(got);
        directUp = 0;
        H.dropEvery = 0;
    }

    // 4. Retail episode we have: no question asked. One we lack: fails.
    write_file("Episode/JK1MP.GOB", a, 10);
    H.infoReqs = 0;
    jkx_StartMap("JK1MP", "gob");
    CHECK(jkx_Tick() == JKX_READY && H.infoReqs == 0);
    jkx_StartMap("JK1CTF", "gob");
    CHECK(jkx_Tick() == JKX_FAILED && !strcmp(jkx_Reason(), "retail-missing"));

    // 5. A host that never answers: with our own copy we go ahead (6 s), without it fail (20 s).
    H.answer = 0;
    jkx_StartMap("mymap", "gob");
    CHECK(run() == JKX_READY);
    jkx_StartMap("nosuch", "gob");
    CHECK(run() == JKX_FAILED && !strcmp(jkx_Reason(), "no-answer"));
    CHECK(H.infoReqs >= 4);
    H.answer = 1;

    // 6. Host says it can't send it, other name, bad name.
    host_offer(JKX_KIND_MAP, "other", "gob", map, mapLen, refs, 2);
    jkx_StartMap("mymap2", "gob");
    CHECK(run() == JKX_FAILED && !strcmp(jkx_Reason(), "other-map"));
    jkx_StartMap("../evil", "gob");
    CHECK(jkx_Tick() == JKX_FAILED && !strcmp(jkx_Reason(), "bad-name"));
    snprintf(H.info, sizeof(H.info), "{\"status\":\"retail-only\",\"name\":\"x\",\"ext\":\"gob\"}");
    jkx_StartMap("x", "gob");
    CHECK(run() == JKX_FAILED && !strcmp(jkx_Reason(), "retail-only") && strstr(jkx_Message(), "original game files"));

    // 7. A reference to a retail file the joiner doesn't have.
    {
        range bad[] = { { offs[1], sizeof(b) } };   // B isn't in our Res2.gob
        host_offer(JKX_KIND_MAP, "mymap3", "gob", map, mapLen, bad, 1);
        jkx_StartMap("mymap3", "gob");
        CHECK(run() == JKX_FAILED && !strcmp(jkx_Reason(), "missing-retail"));
    }

    // 8. A mod: the pack of a folder (with a retail copy inside), unpacked into
    //    netmods/ and giving the same fingerprint.
    {
        uint8_t fp[32], pfp[32];
        uint32_t packLen, poffs[3];
        mkdir("src", 0755); mkdir("src/cog", 0755); mkdir("src/mat", 0755);
        write_file("src/cog/a.cog", a, sizeof(a));
        write_file("src/mat/sab.mat", b, 1234);
        write_file("src/readme.txt", c, 10);   // not packed
        CHECK(jkmod_Fingerprint("src", fp));
        gobfile pf[] = { { "cog/a.cog", a, sizeof(a) }, { "mat/sab.mat", b, 1234 } };
        uint8_t* pack = make_gob(pf, 2, &packLen, poffs);
        // The pack layout: header 12, files, table: same as jkmod packs it.
        jkmod_Sha256(pack, packLen, pfp);
        CHECK(!memcmp(fp, pfp, 32));
        range mrefs[] = { { poffs[0], sizeof(a) } };
        host_offer(JKX_KIND_MOD, "Saber Mod", "gob", pack, packLen, mrefs, 1);
        jkx_StartMod(fp, "Saber Mod", "gob");
        CHECK(run() == JKX_READY);
        CHECK(!strncmp(jkx_ResultPath(), "netmods/", 8));
        uint8_t got[32];
        CHECK(jkmod_Fingerprint(jkx_ResultPath(), got) && !memcmp(got, fp, 32));
        char found[256];
        CHECK(jkmod_FindLocal(fp, found, sizeof(found)) && !strcmp(found, jkx_ResultPath()));

        // Another mod than the one the lobby named.
        fp[0] ^= 1;
        jkx_StartMod(fp, "Saber Mod", "gob");
        CHECK(run() == JKX_FAILED && !strcmp(jkx_Reason(), "other-mod"));
        // A JK host for a MotS joiner.
        fp[0] ^= 1;
        jkx_StartMod(fp, "Saber Mod", "goo");
        CHECK(run() == JKX_FAILED && !strcmp(jkx_Reason(), "other-game"));

        // A pack with a name that would leave the folder.
        gobfile ef[] = { { "../evil.cog", a, 10 } };
        uint8_t* evil = make_gob(ef, 1, &packLen, NULL);
        jkmod_Sha256(evil, packLen, fp);
        host_offer(JKX_KIND_MOD, "Evil", "gob", evil, packLen, NULL, 0);
        jkx_StartMod(fp, "Evil", "gob");
        CHECK(run() == JKX_FAILED && !strcmp(jkx_Reason(), "bad-files"));
        free(pack); free(evil);
    }

    // 10. Our host side: offers our episode and mod, answers our joiner side.
    {
        uint8_t fp[32], got[32];
        size_t n2;
        loopback = 1;
        H.answer = 0;
        uint32_t hmapLen;
        gobfile mf[] = { { "cog/a.cog", a, sizeof(a) }, { "jkl/host.jkl", b, 40000 }, { "mat/c.mat", c, sizeof(c) } };
        uint8_t* hmap = make_gob(mf, 3, &hmapLen, NULL);
        write_file("Episode/HostMap.gob", hmap, hmapLen);
        mkdir("hostmod", 0755); mkdir("hostmod/cog", 0755);
        write_file("hostmod/cog/a.cog", a, sizeof(a));      // a retail copy: sent as a reference
        write_file("hostmod/cog/new.cog", b, 5000);
        {
            gobfile inner[] = { { "mat/c.mat", c, sizeof(c) }, { "mat/x.mat", b + 9000, 700 } };
            uint32_t innerLen;
            uint8_t* ig = make_gob(inner, 2, &innerLen, NULL);   // a GOB inside the mod, with a retail file in it
            write_file("hostmod/extra.gob", ig, innerLen);
            free(ig);
        }
        CHECK(jkmod_Fingerprint("hostmod", fp));
        jkx_HostOffer("HostMap", "gob", "hostmod", 0, fp, "Host \"Mod\"");

        // The joiner hasn't got the map (the host read it already).
        rename("Episode/HostMap.gob", "hostmap-original.gob");
        hostChunkFrames = hostBlockFrames = 0;
        jkx_StartMap("HostMap", "gob");
        CHECK(run() == JKX_READY && hostBlockFrames > 0);   // our host says z: blocks
        uint8_t* gotMap = read_file("Episode/HostMap.gob", &n2);
        CHECK(gotMap && n2 == hmapLen && !memcmp(gotMap, hmap, hmapLen));
        free(gotMap);
        free(hmap);

        // The mod, in blocks over the direct channel.
        directUp = 1;
        hostChunkFrames = hostBlockFrames = 0;
        jkx_StartMod(fp, "Host Mod", "gob");
        CHECK(run() == JKX_READY && hostBlockFrames > 0);
        CHECK(jkmod_Fingerprint(jkx_ResultPath(), got) && !memcmp(got, fp, 32));
        directUp = 0;

        // An original episode isn't offered; a missing one is "none".
        jkx_HostOffer("NoSuchMap", "gob", NULL, 0, NULL, NULL);
        jkx_StartMap("NoSuchMap", "gob");
        CHECK(run() == JKX_FAILED && !strcmp(jkx_Reason(), "none"));
        // No mod offered: a mod request goes unanswered.
        jkx_StartMod(fp, "Host Mod", "gob");
        CHECK(run() == JKX_FAILED && !strcmp(jkx_Reason(), "no-answer"));

        // A mod made only of original files isn't sent.
        mkdir("retailmod", 0755);
        write_file("retailmod/a.cog", a, sizeof(a));
        CHECK(jkmod_Fingerprint("retailmod", fp));
        jkx_HostOffer("", "gob", "retailmod", 0, fp, "Retail");
        jkx_StartMod(fp, "Retail", "gob");
        CHECK(run() == JKX_FAILED && !strcmp(jkx_Reason(), "retail-only"));

        // Files changed after the fingerprint.
        CHECK(jkmod_Fingerprint("hostmod", fp));
        write_file("hostmod/cog/new.cog", b, 5001);
        jkx_HostOffer("", "gob", "hostmod", 0, fp, "Host Mod");
        jkx_StartMod(fp, "Host Mod", "gob");
        CHECK(run() == JKX_FAILED && !strcmp(jkx_Reason(), "changed"));

        // A one-file mod (a GOB in mods/): its pack holds just that file.
        mkdir("mods", 0755);
        write_file("mods/Sabers.gob", b + 100, 3000);
        CHECK(jkmod_FingerprintFile("mods/Sabers.gob", fp));
        jkx_HostOffer("", "gob", "mods/Sabers.gob", 1, fp, "Sabers");
        jkx_StartMod(fp, "Sabers", "gob");
        CHECK(run() == JKX_READY);
        {
            char p2[600];
            snprintf(p2, sizeof(p2), "%s/Sabers.gob", jkx_ResultPath());
            uint8_t* g = read_file(p2, &n2);
            CHECK(g && n2 == 3000 && !memcmp(g, b + 100, 3000));
            free(g);
        }
        // noshare.txt holds a map and mods back; other names are still sent.
        {
            static const char list[] = "# kept to myself\r\n  episode/HOSTMAP.GOB \n\nsab*\nhostmod/\n";
            write_file("NoShare.txt", (const uint8_t*)list, sizeof(list) - 1);
        }
        jkx_HostOffer("HostMap", "gob", "mods/Sabers.gob", 1, fp, "Sabers");   // held back before it's even read
        remove("Episode/HostMap.gob");   // the copy downloaded above
        jkx_StartMap("HostMap", "gob");
        CHECK(run() == JKX_FAILED && !strcmp(jkx_Reason(), "not-shared"));
        jkx_StartMod(fp, "Sabers", "gob");
        CHECK(run() == JKX_FAILED && !strcmp(jkx_Reason(), "not-shared"));
        CHECK(jkmod_Fingerprint("hostmod", fp));
        jkx_HostOffer("", "gob", "hostmod", 0, fp, "Host Mod");
        jkx_StartMod(fp, "Host Mod", "gob");
        CHECK(run() == JKX_FAILED && !strcmp(jkx_Reason(), "not-shared"));
        {
            static const char list[] = "hostmod2\nhost?ap.gob.bak\n";
            write_file("NoShare.txt", (const uint8_t*)list, sizeof(list) - 1);
        }
        jkx_HostOffer("", "gob", "hostmod", 0, fp, "Host Mod");
        jkx_StartMod(fp, "Host Mod", "gob");
        CHECK(run() == JKX_READY);
        remove("NoShare.txt");

        // Several GOBs in mods/ and resource/ are one mod, packed as a folder holding them
        // (the browser's mods ticked together, or such a mod downloaded into netmods/).
        // Retail GOBs and the browser's graphics packs aren't part of it.
        {
            uint8_t setFp[32], dirFp[32], got[32];
            write_file("mods/Extra.gob", b + 200, 2000);
            write_file("mods/JKE.GOB", b + 300, 500);          // a graphics pack: each player's own
            write_file("resource/Res2.gob", b + 400, 600);     // retail
            CHECK(jkmod_FindAlwaysLoaded(0, NULL, 0) == 2);
            CHECK(jkmod_FingerprintAlwaysLoaded(0, setFp));
            mkdir("setcopy", 0755);
            write_file("setcopy/Sabers.gob", b + 100, 3000);
            write_file("setcopy/Extra.gob", b + 200, 2000);
            CHECK(jkmod_Fingerprint("setcopy", dirFp) && !memcmp(setFp, dirFp, 32));
            jkx_HostOffer("", "gob", "mods+resource", JKMOD_SRC_ALWAYS_JK, setFp, "Extra + Sabers");
            jkx_StartMod(setFp, "Extra + Sabers", "gob");
            CHECK(run() == JKX_READY);
            CHECK(jkmod_Fingerprint(jkx_ResultPath(), got) && !memcmp(got, setFp, 32));
            // Held back when noshare.txt lists any of them.
            {
                static const char list[] = "extra.gob\n";
                write_file("NoShare.txt", (const uint8_t*)list, sizeof(list) - 1);
            }
            jkx_HostOffer("", "gob", "mods+resource", JKMOD_SRC_ALWAYS_JK, setFp, "Extra + Sabers");
            jkx_StartMod(setFp, "Extra + Sabers", "gob");
            CHECK(run() == JKX_FAILED && !strcmp(jkx_Reason(), "not-shared"));
            remove("NoShare.txt");
            remove("mods/Extra.gob");
            remove("mods/JKE.GOB");
            remove("resource/Res2.gob");
        }

        jkx_HostWithdraw();
        loopback = 0;
        H.answer = 1;
    }

    // 9. Cancel.
    host_offer(JKX_KIND_MAP, "mymap4", "gob", map, mapLen, refs, 2);
    jkx_StartMap("mymap4", "gob");
    jkx_Tick();
    jkx_Cancel();
    CHECK(jkx_Tick() == JKX_FAILED && !strcmp(jkx_Reason(), "cancelled") && !jkx_Message()[0]);

    // 10. Asking first: stops once the host has said what it sends. No gives up quietly,
    //     yes downloads, and a copy that's already the host's asks nothing.
    jkx_SetAskFirst(1);
    host_offer(JKX_KIND_MAP, "mymap5", "gob", map, mapLen, refs, 0); // whole file: tests above changed the retail GOBs
    jkx_StartMap("mymap5", "gob");
    CHECK(run() == JKX_ASK && strstr(jkx_AskText(), "mymap5") && strstr(jkx_AskText(), "MB"));
    jkx_Answer(0);
    CHECK(jkx_Tick() == JKX_FAILED && !strcmp(jkx_Reason(), "declined") && !jkx_Message()[0]);
    CHECK(read_file("Episode/mymap5.gob", &n) == NULL);
    jkx_StartMap("mymap5", "gob");
    CHECK(run() == JKX_ASK);
    jkx_Answer(1);
    CHECK(run() == JKX_READY);
    {
        uint8_t* got = read_file("Episode/mymap5.gob", &n);
        CHECK(got && n == mapLen && !memcmp(got, map, mapLen));
        free(got);
    }
    jkx_StartMap("mymap5", "gob");
    CHECK(run() == JKX_READY);
    jkx_SetAskFirst(0);

    free(map); free(retail);
    printf(failures ? "\n%d failed\n" : "all passed\n", failures);
    return failures != 0;
}
