#ifndef _JKMOD_H
#define _JKMOD_H

// Mod fingerprints and lobby tails, compatible with the jkdf2mp browser build
// (jkmod.c). Used by the web lobby transport and stdComm_dplay.c.

#include <stddef.h>
#include <stdint.h>

#define JKMOD_MAX_NAME     63
#define JKMOD_PACK_FP      16
#define JKMOD_MAX_TAIL_HEX (2 * (7 + 32 + JKMOD_PACK_FP + JKMOD_MAX_NAME))

// Tail flags (mods.js TAIL_MOD / TAIL_PACK).
#define JKMOD_TAIL_MOD     1
#define JKMOD_TAIL_PACK    2

// The MotS Remastered pack, which the browser build treats as part of the
// game rather than as a mod (mods.js, index.html ALWAYS_LOADED).
#define JKMOD_PACK_FILE    "jkmremasteredassets.goo"

// SHA-256 of the folder packed as the browser build packs it. 0 on failure.
int  jkmod_Fingerprint(const char* dir, uint8_t out[32]);
int  jkmod_FingerprintCached(const char* dir, uint8_t out[32]);
// A single file as a mod (a GOB the engine always loads), as the browser build counts it.
int  jkmod_FingerprintFile(const char* path, uint8_t out[32]);
int  jkmod_FingerprintFileCached(const char* path, uint8_t out[32]);
// Non-original GOBs in mods/ and resource/ (not the Remastered pack or the browser's
// graphics packs JKE/JKR/JK HD Models); returns the count, first path copied out.
int  jkmod_FindAlwaysLoaded(int mots, char* firstPath, size_t len);
// The i-th of them in pack order (by file name). 0 past the end.
int  jkmod_GetAlwaysLoaded(int mots, int i, char* path, size_t len);
// Several of them as one mod, packed like a folder holding them (uncached).
int  jkmod_FingerprintAlwaysLoaded(int mots, uint8_t out[32]);

// The Remastered pack in mods/ or resource/ (that order, as mods.js looks). 1 if found.
int  jkmod_FindRemasterPack(char* outPath, size_t len);
// Its fingerprint: the first 16 bytes of the file's SHA-256. Cached. 0 on failure.
int  jkmod_PackFingerprintCached(const char* path, uint8_t out[JKMOD_PACK_FP]);

// "JKMD" tail (version 2), hex-encoded, for a lobby entry: the mod's fp and
// name, or NULL for none, and the pack's fingerprint, or NULL. "" for neither.
void jkmod_MakeTail(const uint8_t* fp, const char* name, const uint8_t* packFp, char* outHex, size_t outLen);
// -1: unreadable tail; otherwise JKMOD_TAIL_* flags of what the host runs (0:
// no tail), with fp+name or packFp filled for each. Version-1 tails (older
// desktop builds) read as a mod.
int  jkmod_ParseTail(const char* hex, uint8_t fp[32], char* name, size_t nameLen, uint8_t packFp[JKMOD_PACK_FP]);

// A path a pack may hold (mods.js validRel): relative, '/'-separated, at most
// 8 folders deep, printable ASCII, nothing Windows wouldn't allow.
int  jkmod_ValidRel(const char* rel);

// A folder under expansions/, mods/ or netmods/ with this fingerprint. 1 if found.
int  jkmod_FindLocal(const uint8_t fp[32], char* outPath, size_t outLen);

const char* jkmod_BaseName(const char* path);

// The pack itself in memory (to send it): of a folder, or of one file (isFile).
// NULL on success, else a reason as mods.js names it: "too-many-files"
// (over maxFiles), "too-big" (files over maxData bytes), "changed" (a file
// changed while reading), "error". Free *outBytes and *outEntries.
// isFile: 0 a folder, 1 one file, or JKMOD_SRC_ALWAYS_JK/_MOTS all the always-loaded GOBs (path unused).
#define JKMOD_SRC_ALWAYS_JK   2
#define JKMOD_SRC_ALWAYS_MOTS 3
typedef struct { uint32_t offset, size; char name[128]; } jkmod_packEntry;
const char* jkmod_Pack(const char* path, int isFile, int maxFiles, uint64_t maxData,
                       uint8_t** outBytes, uint32_t* outLen, jkmod_packEntry** outEntries, int* outCount);

// SHA-256 of a buffer, and of size bytes of a file from offset (0 on a read error).
void jkmod_Sha256(const void* data, size_t n, uint8_t out[32]);
int  jkmod_Sha256FileRange(const char* path, uint64_t offset, uint64_t size, uint8_t out[32]);

#endif
