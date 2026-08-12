#include "efs/checksum.h"
#include "blake3.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void efs_hash(const void *data, size_t len, uint8_t out[EFS_HASH_SIZE])
{
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, len);
    blake3_hasher_finalize(&hasher, out, EFS_HASH_SIZE);
}

/* pthread_once target for the cached 64 KiB zero-fragment digest. */
static uint8_t g_zero_frag_digest[EFS_HASH_SIZE];
static void efs_zero_frag_digest_init(void)
{
    static const uint8_t zeros[EFS_FRAGMENT_SIZE];
    efs_hash(zeros, EFS_FRAGMENT_SIZE, g_zero_frag_digest);
}

void efs_hash_zero_fragment_len(size_t frag_len, uint8_t out[EFS_HASH_SIZE])
{
    /* Cache default meta/data fragment size (64 KiB). pthread_once makes the
     * lazy init thread-safe (the previous racy `ready` flag could let two
     * threads hash simultaneously and one read a half-written digest). */
    static pthread_once_t once_default = PTHREAD_ONCE_INIT;
    if (frag_len == EFS_FRAGMENT_SIZE) {
        pthread_once(&once_default, efs_zero_frag_digest_init);
        memcpy(out, g_zero_frag_digest, EFS_HASH_SIZE);
        return;
    }
    if (frag_len == 0 || frag_len > EFS_MAX_FRAGMENT_SIZE) {
        memset(out, 0, EFS_HASH_SIZE);
        return;
    }
    uint8_t *zeros = calloc(1, frag_len);
    if (!zeros) {
        memset(out, 0, EFS_HASH_SIZE);
        return;
    }
    efs_hash(zeros, frag_len, out);
    free(zeros);
}

void efs_hash_zero_fragment(uint8_t out[EFS_HASH_SIZE])
{
    efs_hash_zero_fragment_len(EFS_FRAGMENT_SIZE, out);
}

int efs_bytes_are_zero(const void *data, size_t len)
{
    if (!data)
        return 0;
    if (len == 0)
        return 1;
    /* memcmp vs a BSS zero page is far faster than a hand-rolled word loop
     * (glibc uses AVX). Chunk/fragment sizes are the PUT/GET hot path. */
    static const uint8_t zero_frag[EFS_FRAGMENT_SIZE];
    static const uint8_t zero_chunk[EFS_DEFAULT_CHUNK_SIZE];
    if (len == EFS_FRAGMENT_SIZE)
        return memcmp(data, zero_frag, EFS_FRAGMENT_SIZE) == 0;
    if (len == EFS_DEFAULT_CHUNK_SIZE)
        return memcmp(data, zero_chunk, EFS_DEFAULT_CHUNK_SIZE) == 0;

    const uint8_t *p = data;
    while (len >= EFS_FRAGMENT_SIZE) {
        if (memcmp(p, zero_frag, EFS_FRAGMENT_SIZE) != 0)
            return 0;
        p += EFS_FRAGMENT_SIZE;
        len -= EFS_FRAGMENT_SIZE;
    }
    if (len > 0 && memcmp(p, zero_frag, len) != 0)
        return 0;
    return 1;
}

const uint8_t *efs_zero_bytes(size_t len)
{
    static const uint8_t zero_default[EFS_FRAGMENT_SIZE];
    static uint8_t *zero_big;
    static size_t zero_big_len;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

    if (len == 0)
        return zero_default;
    if (len <= EFS_FRAGMENT_SIZE)
        return zero_default;
    if (len > EFS_MAX_FRAGMENT_SIZE)
        return NULL;

    pthread_mutex_lock(&mu);
    if (!zero_big || zero_big_len < len) {
        uint8_t *fresh = calloc(1, len);
        if (!fresh) {
            pthread_mutex_unlock(&mu);
            return NULL;
        }
        free(zero_big);
        zero_big = fresh;
        zero_big_len = len;
    }
    pthread_mutex_unlock(&mu);
    return zero_big;
}

void efs_hash_to_hex(const uint8_t hash[EFS_HASH_SIZE], char hex[EFS_HASH_SIZE * 2 + 1])
{
    static const char hex_chars[] = "0123456789abcdef";
    for (size_t i = 0; i < EFS_HASH_SIZE; i++) {
        hex[i * 2] = hex_chars[(hash[i] >> 4) & 0xF];
        hex[i * 2 + 1] = hex_chars[hash[i] & 0xF];
    }
    hex[EFS_HASH_SIZE * 2] = '\0';
}

int efs_hash_from_hex(const char hex[EFS_HASH_SIZE * 2 + 1], uint8_t hash[EFS_HASH_SIZE])
{
    if (!hex || !hash)
        return -1;

    for (size_t i = 0; i < EFS_HASH_SIZE; i++) {
        int hi = -1, lo = -1;
        char c = hex[i * 2];
        if (c >= '0' && c <= '9') hi = c - '0';
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        else return -1;

        c = hex[i * 2 + 1];
        if (c >= '0' && c <= '9') lo = c - '0';
        else if (c >= 'a' && c <= 'f') lo = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') lo = c - 'A' + 10;
        else return -1;

        hash[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}
