// Lobby tail tests for jkmod.c, mirroring packaging/wasm/test/mods-test.mjs in
// github.com/tristamus/OpenJKDF2 (tail version 2, since ffd26b53).
//
// cc -std=gnu11 -DPLATFORM_DPLAY -Isrc/Platform/Networking/DPlay src/Tests/jkmod_test.c src/Platform/Networking/DPlay/jkmod.c && ./a.out

#include "jkmod.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void hex(const uint8_t* b, size_t n, char* out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2*i] = d[b[i] >> 4]; out[2*i+1] = d[b[i] & 15]; }
    out[2*n] = 0;
}

int main(void)
{
    uint8_t fp[32], pack[JKMOD_PACK_FP], gotFp[32], gotPack[JKMOD_PACK_FP];
    char t[JKMOD_MAX_TAIL_HEX + 1], name[JKMOD_MAX_NAME + 1], want[512], fpHex[65], packHex[33], bad[600];
    char longName[201];

    for (int i = 0; i < 32; i++) fp[i] = (uint8_t)(0xA0 + i);
    for (int i = 0; i < JKMOD_PACK_FP; i++) pack[i] = (uint8_t)(0x10 + i);
    hex(fp, 32, fpHex);
    hex(pack, JKMOD_PACK_FP, packHex);

    // Nothing to name: no tail.
    jkmod_MakeTail(NULL, NULL, NULL, t, sizeof(t));
    CHECK(!strcmp(t, ""));
    CHECK(jkmod_ParseTail("", gotFp, name, sizeof(name), gotPack) == 0);

    // Mod only: "JKMD" 02 01 <len> <fp> <name>.
    jkmod_MakeTail(fp, "Saber Battle X", NULL, t, sizeof(t));
    snprintf(want, sizeof(want), "4a4b4d4402010e%s", fpHex);
    for (const char* p = "Saber Battle X"; *p; p++) snprintf(want + strlen(want), 3, "%02x", (unsigned char)*p);
    CHECK(!strcmp(t, want));
    memset(gotFp, 0, 32);
    CHECK(jkmod_ParseTail(t, gotFp, name, sizeof(name), gotPack) == JKMOD_TAIL_MOD);
    CHECK(!memcmp(gotFp, fp, 32));
    CHECK(!strcmp(name, "Saber Battle X"));

    // Truncated, extended, non-hex, too short: unreadable.
    CHECK(jkmod_ParseTail("00", gotFp, name, sizeof(name), gotPack) < 0);
    snprintf(bad, sizeof(bad), "%.*s", (int)strlen(t) - 2, t);
    CHECK(jkmod_ParseTail(bad, gotFp, name, sizeof(name), gotPack) < 0);
    snprintf(bad, sizeof(bad), "%s00", t);
    CHECK(jkmod_ParseTail(bad, gotFp, name, sizeof(name), gotPack) < 0);
    snprintf(bad, sizeof(bad), "zz%s", t + 2);
    CHECK(jkmod_ParseTail(bad, gotFp, name, sizeof(name), gotPack) < 0);

    // Names: cut to 63, non-ASCII as one '?' per character.
    memset(longName, 'x', 200); longName[200] = 0;
    jkmod_MakeTail(fp, longName, NULL, t, sizeof(t));
    CHECK(jkmod_ParseTail(t, gotFp, name, sizeof(name), gotPack) == JKMOD_TAIL_MOD && strlen(name) == 63);
    jkmod_MakeTail(fp, "caf\xc3\xa9", NULL, t, sizeof(t));
    CHECK(jkmod_ParseTail(t, gotFp, name, sizeof(name), gotPack) == JKMOD_TAIL_MOD && !strcmp(name, "caf?"));
    jkmod_MakeTail(fp, "  ", NULL, t, sizeof(t));
    CHECK(jkmod_ParseTail(t, gotFp, name, sizeof(name), gotPack) == JKMOD_TAIL_MOD && !strcmp(name, "mod"));

    // Pack only: "JKMD" 02 02 00 <pack>.
    jkmod_MakeTail(NULL, NULL, pack, t, sizeof(t));
    snprintf(want, sizeof(want), "4a4b4d44020200%s", packHex);
    CHECK(!strcmp(t, want));
    memset(gotPack, 0, sizeof(gotPack));
    CHECK(jkmod_ParseTail(t, gotFp, name, sizeof(name), gotPack) == JKMOD_TAIL_PACK);
    CHECK(!memcmp(gotPack, pack, JKMOD_PACK_FP) && !strcmp(name, ""));

    // Bad flags (00, 04, and a mod flag without a mod), and a name without a mod.
    {
        const char* flags[] = { "00", "04", "01" };
        for (int i = 0; i < 3; i++)
        {
            snprintf(bad, sizeof(bad), "%.10s%s%s", t, flags[i], t + 12);
            CHECK(jkmod_ParseTail(bad, gotFp, name, sizeof(name), gotPack) < 0);
        }
        snprintf(bad, sizeof(bad), "%.12s03%s414243", t, t + 14);
        CHECK(jkmod_ParseTail(bad, gotFp, name, sizeof(name), gotPack) < 0);
    }

    // Both: "JKMD" 02 03 <len> <fp> <pack> <name>.
    jkmod_MakeTail(fp, "SBX", pack, t, sizeof(t));
    snprintf(want, sizeof(want), "4a4b4d44020303%s%s534258", fpHex, packHex);
    CHECK(!strcmp(t, want));
    CHECK(jkmod_ParseTail(t, gotFp, name, sizeof(name), gotPack) == (JKMOD_TAIL_MOD | JKMOD_TAIL_PACK));
    CHECK(!memcmp(gotFp, fp, 32) && !memcmp(gotPack, pack, JKMOD_PACK_FP) && !strcmp(name, "SBX"));

    // The longest fits the browser engine's WSRELAY_MAX_TAIL_HEX and our buffer.
    jkmod_MakeTail(fp, longName, pack, t, sizeof(t));
    CHECK(strlen(t) <= 256 && strlen(t) == JKMOD_MAX_TAIL_HEX);

    // Version 1 (older desktop builds) still reads as a mod here; the browser rejects it.
    snprintf(want, sizeof(want), "4a4b4d440103%s534258", fpHex);
    CHECK(jkmod_ParseTail(want, gotFp, name, sizeof(name), gotPack) == JKMOD_TAIL_MOD);
    CHECK(!memcmp(gotFp, fp, 32) && !strcmp(name, "SBX"));

    // Pack fingerprint: first 16 bytes of the file's SHA-256 (SHA-256("abc") = ba7816bf...).
    {
        const char* path = "jkmod_test_pack.tmp";
        FILE* f = fopen(path, "wb");
        CHECK(f != NULL);
        if (f)
        {
            fputs("abc", f);
            fclose(f);
            CHECK(jkmod_PackFingerprintCached(path, gotPack));
            hex(gotPack, JKMOD_PACK_FP, packHex);
            CHECK(!strcmp(packHex, "ba7816bf8f01cfea414140de5dae2223"));
            remove(path);
        }
    }

    printf(failures ? "\n%d failed\n" : "all passed\n", failures);
    return failures != 0;
}
