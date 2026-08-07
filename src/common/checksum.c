#include "efs/checksum.h"
#include "blake3.h"
#include <stdio.h>
#include <string.h>

void efs_hash(const void *data, size_t len, uint8_t out[EFS_HASH_SIZE])
{
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, len);
    blake3_hasher_finalize(&hasher, out, EFS_HASH_SIZE);
}

void efs_hash_zero_fragment(uint8_t out[EFS_HASH_SIZE])
{
    static uint8_t cached[EFS_HASH_SIZE];
    static int ready;
    if (!ready) {
        static const uint8_t zeros[EFS_FRAGMENT_SIZE];
        efs_hash(zeros, EFS_FRAGMENT_SIZE, cached);
        ready = 1;
    }
    memcpy(out, cached, EFS_HASH_SIZE);
}

int efs_bytes_are_zero(const void *data, size_t len)
{
    if (!data)
        return 0;
    if (len == EFS_FRAGMENT_SIZE) {
        static const uint8_t zeros[EFS_FRAGMENT_SIZE];
        return memcmp(data, zeros, EFS_FRAGMENT_SIZE) == 0;
    }
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        if (p[i] != 0)
            return 0;
    }
    return 1;
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
