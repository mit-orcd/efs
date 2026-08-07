#include "efs/local_ec.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* GF(256) with irreducible polynomial 0x11d (AES/RAID-6 friendly). */
static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static int gf_ready;

static void gf_init(void)
{
    if (gf_ready)
        return;
    int x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100)
            x ^= 0x11d;
    }
    for (int i = 255; i < 512; i++)
        gf_exp[i] = gf_exp[i - 255];
    gf_log[0] = 0;
    gf_ready = 1;
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0)
        return 0;
    return gf_exp[gf_log[a] + gf_log[b]];
}

static uint8_t gf_div(uint8_t a, uint8_t b)
{
    if (a == 0)
        return 0;
    if (b == 0)
        return 0;
    return gf_exp[gf_log[a] + 255 - gf_log[b]];
}

static uint8_t gf_pow2(int e)
{
    /* 2^e in GF */
    e %= 255;
    if (e < 0)
        e += 255;
    return gf_exp[e];
}

uint32_t efs_local_ec_k(uint32_t n)
{
    if (n <= 1)
        return 1;
    if (n == 3)
        return 2;
    if (n >= 4)
        return n - 2;
    return 0;
}

uint32_t efs_local_ec_m(uint32_t n)
{
    if (n <= 1)
        return 0;
    if (n == 3)
        return 1;
    if (n >= 4)
        return 2;
    return 0;
}

uint32_t efs_local_ec_shard_len(uint32_t n)
{
    if (n <= 1)
        return EFS_FRAGMENT_SIZE;
    uint32_t k = efs_local_ec_k(n);
    if (k == 0)
        return 0;
    /* ceil(frag/k), round up to 4096 for O_DIRECT. */
    uint32_t raw = (EFS_FRAGMENT_SIZE + k - 1) / k;
    uint32_t align = 4096;
    return (raw + align - 1) / align * align;
}

static int encode_plain(const uint8_t *frag, uint32_t frag_len,
                        uint8_t shards[EFS_MAX_STORAGE_PATHS][EFS_LOCAL_EC_MAX_SHARD])
{
    memset(shards[0], 0, EFS_FRAGMENT_SIZE);
    if (frag_len > EFS_FRAGMENT_SIZE)
        return EFS_ERR_INVAL;
    if (frag_len)
        memcpy(shards[0], frag, frag_len);
    return EFS_OK;
}

/* n=3: D0|D1 halves of fragment + P = D0 XOR D1 (32 KiB each). */
static int encode_xor3(const uint8_t *frag, uint32_t frag_len,
                       uint8_t shards[EFS_MAX_STORAGE_PATHS][EFS_LOCAL_EC_MAX_SHARD])
{
    const uint32_t half = EFS_FRAGMENT_SIZE / 2;
    memset(shards[0], 0, half);
    memset(shards[1], 0, half);
    if (frag_len > half) {
        memcpy(shards[0], frag, half);
        memcpy(shards[1], frag + half, frag_len - half);
    } else if (frag_len) {
        memcpy(shards[0], frag, frag_len);
    }
    for (uint32_t i = 0; i < half; i++)
        shards[2][i] = shards[0][i] ^ shards[1][i];
    return EFS_OK;
}

/* RAID-6 style: k data shards + P (XOR) + Q (GF syndrome with g=2). */
static int encode_rs(uint32_t n, const uint8_t *frag, uint32_t frag_len,
                     uint8_t shards[EFS_MAX_STORAGE_PATHS][EFS_LOCAL_EC_MAX_SHARD])
{
    gf_init();
    uint32_t k = efs_local_ec_k(n);
    uint32_t slen = efs_local_ec_shard_len(n);
    if (k < 2 || k + 2 != n || slen == 0 || slen > EFS_LOCAL_EC_MAX_SHARD)
        return EFS_ERR_INVAL;

    for (uint32_t i = 0; i < n; i++)
        memset(shards[i], 0, slen);

    /* Split fragment across k data shards. */
    uint32_t off = 0;
    for (uint32_t di = 0; di < k; di++) {
        uint32_t take = frag_len - off;
        if (take > slen)
            take = slen;
        if (off < frag_len && take)
            memcpy(shards[di], frag + off, take);
        off += take;
    }

    uint32_t p_idx = k;
    uint32_t q_idx = k + 1;
    for (uint32_t i = 0; i < slen; i++) {
        uint8_t p = 0;
        uint8_t q = 0;
        for (uint32_t di = 0; di < k; di++) {
            uint8_t d = shards[di][i];
            p ^= d;
            q ^= gf_mul(gf_pow2((int)di), d);
        }
        shards[p_idx][i] = p;
        shards[q_idx][i] = q;
    }
    return EFS_OK;
}

int efs_local_ec_encode(uint32_t n, const uint8_t *frag, uint32_t frag_len,
                        uint8_t shards[EFS_MAX_STORAGE_PATHS][EFS_LOCAL_EC_MAX_SHARD])
{
    if (!frag || !shards || frag_len > EFS_FRAGMENT_SIZE)
        return EFS_ERR_INVAL;
    if (n == 1)
        return encode_plain(frag, frag_len, shards);
    if (n == 3)
        return encode_xor3(frag, frag_len, shards);
    if (n >= 4 && n <= EFS_MAX_STORAGE_PATHS)
        return encode_rs(n, frag, frag_len, shards);
    return EFS_ERR_INVAL;
}

static int decode_plain(const uint8_t shards[EFS_MAX_STORAGE_PATHS][EFS_LOCAL_EC_MAX_SHARD],
                        const int have[EFS_MAX_STORAGE_PATHS],
                        uint8_t *frag_out, uint32_t frag_len)
{
    if (!have[0])
        return EFS_ERR_DECODE;
    memcpy(frag_out, shards[0], frag_len);
    return EFS_OK;
}

static int decode_xor3(const uint8_t shards[EFS_MAX_STORAGE_PATHS][EFS_LOCAL_EC_MAX_SHARD],
                       const int have[EFS_MAX_STORAGE_PATHS],
                       uint8_t *frag_out, uint32_t frag_len)
{
    const uint32_t half = EFS_FRAGMENT_SIZE / 2;
    uint8_t d0[EFS_FRAGMENT_SIZE / 2];
    uint8_t d1[EFS_FRAGMENT_SIZE / 2];
    int nhave = have[0] + have[1] + have[2];
    if (nhave < 2)
        return EFS_ERR_DECODE;

    if (have[0] && have[1]) {
        memcpy(d0, shards[0], half);
        memcpy(d1, shards[1], half);
    } else if (have[0] && have[2]) {
        memcpy(d0, shards[0], half);
        for (uint32_t i = 0; i < half; i++)
            d1[i] = shards[0][i] ^ shards[2][i];
    } else if (have[1] && have[2]) {
        memcpy(d1, shards[1], half);
        for (uint32_t i = 0; i < half; i++)
            d0[i] = shards[1][i] ^ shards[2][i];
    } else {
        return EFS_ERR_DECODE;
    }

    uint8_t full[EFS_FRAGMENT_SIZE];
    memcpy(full, d0, half);
    memcpy(full + half, d1, half);
    memcpy(frag_out, full, frag_len);
    return EFS_OK;
}

/* Recover up to 2 missing shards among k data + P + Q (RAID-6). */
static int decode_rs(uint32_t n,
                     const uint8_t shards_in[EFS_MAX_STORAGE_PATHS][EFS_LOCAL_EC_MAX_SHARD],
                     const int have_in[EFS_MAX_STORAGE_PATHS],
                     uint8_t *frag_out, uint32_t frag_len)
{
    gf_init();
    uint32_t k = efs_local_ec_k(n);
    uint32_t slen = efs_local_ec_shard_len(n);
    if (k + 2 != n)
        return EFS_ERR_INVAL;

    /* Heap: EFS_MAX_STORAGE_PATHS × max shard ≈ hundreds of KiB. */
    uint8_t (*shards)[EFS_LOCAL_EC_MAX_SHARD] =
        malloc(EFS_MAX_STORAGE_PATHS * EFS_LOCAL_EC_MAX_SHARD);
    int *have = malloc(EFS_MAX_STORAGE_PATHS * sizeof(int));
    if (!shards || !have) {
        free(shards);
        free(have);
        return EFS_ERR_NOMEM;
    }
    memcpy(shards, shards_in, EFS_MAX_STORAGE_PATHS * EFS_LOCAL_EC_MAX_SHARD);
    memcpy(have, have_in, EFS_MAX_STORAGE_PATHS * sizeof(int));

    int data_ok = 1;
    for (uint32_t di = 0; di < k; di++) {
        if (!have[di]) {
            data_ok = 0;
            break;
        }
    }
    if (data_ok)
        goto assemble;

    int missing[EFS_MAX_STORAGE_PATHS];
    int nmiss = 0;
    int nhave = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (have[i])
            nhave++;
        else if (nmiss < (int)EFS_MAX_STORAGE_PATHS)
            missing[nmiss++] = (int)i;
    }
    if (nhave < (int)k || nmiss > 2)
        goto fail;

    uint32_t p_idx = k;
    uint32_t q_idx = k + 1;

    for (uint32_t i = 0; i < slen; i++) {
        if (nmiss == 1) {
            int m = missing[0];
            if ((uint32_t)m == p_idx) {
                uint8_t p = 0;
                for (uint32_t di = 0; di < k; di++)
                    p ^= shards[di][i];
                shards[p_idx][i] = p;
            } else if ((uint32_t)m == q_idx) {
                uint8_t q = 0;
                for (uint32_t di = 0; di < k; di++)
                    q ^= gf_mul(gf_pow2((int)di), shards[di][i]);
                shards[q_idx][i] = q;
            } else {
                /* One data shard missing: recover from P if present, else Q. */
                if (have[p_idx]) {
                    uint8_t p = shards[p_idx][i];
                    for (uint32_t di = 0; di < k; di++) {
                        if ((int)di != m)
                            p ^= shards[di][i];
                    }
                    shards[m][i] = p;
                } else if (have[q_idx]) {
                    uint8_t q = shards[q_idx][i];
                    for (uint32_t di = 0; di < k; di++) {
                        if ((int)di != m)
                            q ^= gf_mul(gf_pow2((int)di), shards[di][i]);
                    }
                    shards[m][i] = gf_div(q, gf_pow2(m));
                } else {
                    goto fail;
                }
            }
            have[m] = 1;
        } else if (nmiss == 2) {
            int m0 = missing[0];
            int m1 = missing[1];
            /* Prefer recovering two data disks via P+Q. */
            if ((uint32_t)m0 < k && (uint32_t)m1 < k && have[p_idx] && have[q_idx]) {
                uint8_t p = shards[p_idx][i];
                uint8_t q = shards[q_idx][i];
                for (uint32_t di = 0; di < k; di++) {
                    if ((int)di != m0 && (int)di != m1) {
                        p ^= shards[di][i];
                        q ^= gf_mul(gf_pow2((int)di), shards[di][i]);
                    }
                }
                /* p = D_m0 ^ D_m1
                 * q = g^m0 * D_m0 ^ g^m1 * D_m1
                 * D_m1 = (q ^ g^m0 * p) / (g^m1 ^ g^m0)
                 */
                uint8_t g0 = gf_pow2(m0);
                uint8_t g1 = gf_pow2(m1);
                uint8_t denom = g0 ^ g1;
                uint8_t d1 = gf_div(q ^ gf_mul(g0, p), denom);
                uint8_t d0 = p ^ d1;
                shards[m0][i] = d0;
                shards[m1][i] = d1;
            } else if ((uint32_t)m0 < k && (uint32_t)m1 == p_idx && have[q_idx]) {
                /* data + P missing: recover data from Q, then P */
                uint8_t q = shards[q_idx][i];
                for (uint32_t di = 0; di < k; di++) {
                    if ((int)di != m0)
                        q ^= gf_mul(gf_pow2((int)di), shards[di][i]);
                }
                shards[m0][i] = gf_div(q, gf_pow2(m0));
                uint8_t p = 0;
                for (uint32_t di = 0; di < k; di++)
                    p ^= shards[di][i];
                shards[p_idx][i] = p;
            } else if ((uint32_t)m1 < k && (uint32_t)m0 == p_idx && have[q_idx]) {
                uint8_t q = shards[q_idx][i];
                for (uint32_t di = 0; di < k; di++) {
                    if ((int)di != m1)
                        q ^= gf_mul(gf_pow2((int)di), shards[di][i]);
                }
                shards[m1][i] = gf_div(q, gf_pow2(m1));
                uint8_t p = 0;
                for (uint32_t di = 0; di < k; di++)
                    p ^= shards[di][i];
                shards[p_idx][i] = p;
            } else if ((uint32_t)m0 < k && (uint32_t)m1 == q_idx && have[p_idx]) {
                uint8_t p = shards[p_idx][i];
                for (uint32_t di = 0; di < k; di++) {
                    if ((int)di != m0)
                        p ^= shards[di][i];
                }
                shards[m0][i] = p;
                uint8_t q = 0;
                for (uint32_t di = 0; di < k; di++)
                    q ^= gf_mul(gf_pow2((int)di), shards[di][i]);
                shards[q_idx][i] = q;
            } else if ((uint32_t)m1 < k && (uint32_t)m0 == q_idx && have[p_idx]) {
                uint8_t p = shards[p_idx][i];
                for (uint32_t di = 0; di < k; di++) {
                    if ((int)di != m1)
                        p ^= shards[di][i];
                }
                shards[m1][i] = p;
                uint8_t q = 0;
                for (uint32_t di = 0; di < k; di++)
                    q ^= gf_mul(gf_pow2((int)di), shards[di][i]);
                shards[q_idx][i] = q;
            } else if ((uint32_t)m0 == p_idx && (uint32_t)m1 == q_idx) {
                uint8_t p = 0, q = 0;
                for (uint32_t di = 0; di < k; di++) {
                    p ^= shards[di][i];
                    q ^= gf_mul(gf_pow2((int)di), shards[di][i]);
                }
                shards[p_idx][i] = p;
                shards[q_idx][i] = q;
            } else {
                goto fail;
            }
        } else {
            goto fail;
        }
    }

assemble:
    /* Assemble fragment from data shards. */
    {
        uint8_t full[EFS_FRAGMENT_SIZE];
        memset(full, 0, sizeof(full));
        uint32_t off = 0;
        for (uint32_t di = 0; di < k && off < EFS_FRAGMENT_SIZE; di++) {
            uint32_t take = EFS_FRAGMENT_SIZE - off;
            if (take > slen)
                take = slen;
            memcpy(full + off, shards[di], take);
            off += take;
        }
        memcpy(frag_out, full, frag_len);
    }
    free(shards);
    free(have);
    return EFS_OK;

fail:
    free(shards);
    free(have);
    return EFS_ERR_DECODE;
}

int efs_local_ec_decode(uint32_t n,
                        const uint8_t shards[EFS_MAX_STORAGE_PATHS][EFS_LOCAL_EC_MAX_SHARD],
                        const int have[EFS_MAX_STORAGE_PATHS],
                        uint8_t *frag_out, uint32_t frag_len)
{
    if (!shards || !have || !frag_out || frag_len > EFS_FRAGMENT_SIZE)
        return EFS_ERR_INVAL;
    if (n == 1)
        return decode_plain(shards, have, frag_out, frag_len);
    if (n == 3)
        return decode_xor3(shards, have, frag_out, frag_len);
    if (n >= 4 && n <= EFS_MAX_STORAGE_PATHS)
        return decode_rs(n, shards, have, frag_out, frag_len);
    return EFS_ERR_INVAL;
}
