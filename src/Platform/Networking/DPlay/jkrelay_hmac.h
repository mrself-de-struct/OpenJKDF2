#ifndef _JKRELAY_HMAC_H
#define _JKRELAY_HMAC_H

// HMAC-SHA256 (RFC 2104) for rcon (jkrelay_proto.h, "Rcon"), on jkmod.c's SHA-256.
// Messages are short (a nonce and a console command).

#include <stdint.h>
#include <string.h>

void jkmod_Sha256(const void* data, size_t n, uint8_t out[32]);   // jkmod.c

static int jkr_HmacSha256(const char* key, const uint8_t* msg, size_t msgLen, uint8_t out[32])
{
    uint8_t k[64], inner[64 + 512], outer[64 + 32];
    size_t keyLen = strlen(key);
    if (msgLen > 512) return 0;
    memset(k, 0, sizeof(k));
    if (keyLen > 64) jkmod_Sha256(key, keyLen, k);
    else memcpy(k, key, keyLen);
    for (int i = 0; i < 64; i++) { inner[i] = k[i] ^ 0x36; outer[i] = k[i] ^ 0x5c; }
    memcpy(inner + 64, msg, msgLen);
    jkmod_Sha256(inner, 64 + msgLen, outer + 64);
    jkmod_Sha256(outer, sizeof(outer), out);
    return 1;
}

#endif // _JKRELAY_HMAC_H
