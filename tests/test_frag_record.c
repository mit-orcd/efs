/* I25 identity-bound fragment record (fragment format 2): encode/verify. */
#include "efs/frag_record.h"
#include "efs/checksum.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct efs_frag_ident base(uint32_t kind)
{
    struct efs_frag_ident id = {
        .kind = kind, .space = kind == EFS_FRAG_KIND_CHUNK ? 1 : 7,
        .fragment_index = 2, .chunk_index = kind == EFS_FRAG_KIND_CHUNK ? 37 : 0,
        .payload_len = 4096, .coding_profile = EFS_FRAG_PROFILE_K2F1,
        .object = 0x1234567890ull, .generation = 0xfeedfacecafebeefull,
    };
    return id;
}

static void check_kind(uint32_t kind)
{
    static uint8_t payload[4096];
    uint8_t digest[EFS_HASH_SIZE], got[EFS_HASH_SIZE], rec[EFS_FRAG_RECORD_BYTES];
    const char *field;
    for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 7 + kind);
    struct efs_frag_ident id = base(kind);
    efs_hash(payload, sizeof(payload), digest);
    assert(efs_frag_record_encode(&id, digest, rec) == EFS_OK);

    /* Right identity passes and returns the payload (wire) digest. */
    assert(efs_frag_verify(payload, sizeof(payload), rec, sizeof(rec), &id, got, &field) == EFS_OK);
    assert(!memcmp(got, digest, EFS_HASH_SIZE) && !field);
    struct efs_frag_ident parsed;
    assert(efs_frag_record_parse(rec, sizeof(rec), &parsed, NULL) == EFS_OK);
    assert(!memcmp(&parsed, &id, sizeof(id)));

    /* Each identity field wrong is refused as an identity mismatch. */
    for (int f = 0; f < 8; f++) {
        struct efs_frag_ident w = id;
        const char *want;
        switch (f) {
        case 0: w.space ^= 1; want = "space"; break;
        case 1: w.object ^= 1; want = "object"; break;            /* inode / segment id */
        case 2: w.generation ^= 1; want = "generation"; break;    /* chunk/segment generation */
        case 3: w.chunk_index ^= 1; want = "chunk_index"; break;
        case 4: w.fragment_index = (w.fragment_index + 1) % EFS_NUM_FRAGMENTS; want = "fragment_index"; break;
        case 5: w.payload_len += 4096; want = "payload_len"; break;
        case 6: w.kind = kind == EFS_FRAG_KIND_CHUNK ? EFS_FRAG_KIND_SEGMENT : EFS_FRAG_KIND_CHUNK; want = "kind"; break;
        default: w.coding_profile = 2; want = "coding_profile"; break;
        }
        field = NULL;
        memset(got, 0xa5, sizeof(got));
        uint8_t unchanged[EFS_HASH_SIZE];memcpy(unchanged,got,sizeof(got));
        int rc = efs_frag_record_verify(rec, sizeof(rec), &w, got, &field);
        assert(rc == EFS_ERR_FRAG_IDENTITY);
        assert(!memcmp(got,unchanged,sizeof(got)));
        assert(field && !strcmp(field, want));
    }

    /* Flipped and truncated payloads are refused as corruption. */
    payload[100] ^= 1;
    assert(efs_frag_verify(payload, sizeof(payload), rec, sizeof(rec), &id, got, &field) == EFS_ERR_CHECKSUM);
    payload[100] ^= 1;
    assert(efs_frag_verify(payload, sizeof(payload) - 1, rec, sizeof(rec), &id, got, &field) == EFS_ERR_CHECKSUM);
    /* Torn / absent record. */
    assert(efs_frag_verify(payload, sizeof(payload), rec, sizeof(rec) - 1, &id, got, &field) == EFS_ERR_CHECKSUM);
    assert(efs_frag_verify(payload, sizeof(payload), NULL, 0, &id, got, &field) == EFS_ERR_CHECKSUM);

    /* Any flipped identity, digest or bound byte breaks the bound digest:
     * an attacker-free rot of the record cannot silently rename an object. */
    for (unsigned b = 16; b < 120; b++) {
        uint8_t bad[EFS_FRAG_RECORD_BYTES];
        memcpy(bad, rec, sizeof(bad));
        bad[b] ^= 0x10;
        int rc = efs_frag_record_verify(bad, sizeof(bad), &id, got, NULL);
        assert(rc == EFS_ERR_CHECKSUM || (b >= 52 && b < 56 && rc == EFS_ERR_FRAG_FORMAT));
    }
    /* A consistent record re-hashed under another identity still names the
     * other object: binding is by content, not by the record's own hash. */
    {
        struct efs_frag_ident other = id;
        other.object++;
        uint8_t swapped[EFS_FRAG_RECORD_BYTES];
        assert(efs_frag_record_encode(&other, digest, swapped) == EFS_OK);
        assert(efs_frag_verify(payload, sizeof(payload), swapped, sizeof(swapped), &id, got, &field) ==
               EFS_ERR_FRAG_IDENTITY && !strcmp(field, "object"));
    }

    /* Unknown version, magic, kind or record size: format refusal. */
    for (int f = 0; f < 5; f++) {
        uint8_t bad[EFS_FRAG_RECORD_BYTES];
        memcpy(bad, rec, sizeof(bad));
        switch (f) {
        case 0: bad[9] = 3; break;          /* version 3 */
        case 1: bad[9] = 1; break;          /* version 1 */
        case 2: bad[0] = 'X'; break;        /* magic */
        case 3: bad[11] = 9; break;         /* kind */
        default: bad[15] = 64; break;       /* record bytes */
        }
        efs_hash(bad, 88, bad + 88);        /* even with a matching bound digest */
        assert(efs_frag_record_verify(bad, sizeof(bad), &id, got, NULL) == EFS_ERR_FRAG_FORMAT);
    }
    /* The previous on-disk tail (bare 32-byte payload digest, zero padded). */
    {
        uint8_t old[EFS_FRAG_RECORD_BYTES] = {0};
        memcpy(old, digest, EFS_HASH_SIZE);
        assert(efs_frag_verify(payload, sizeof(payload), old, sizeof(old), &id, got, NULL) == EFS_ERR_FRAG_FORMAT);
        uint8_t seg1[EFS_FRAG_RECORD_BYTES] = {0}; /* old segment tail */
        memcpy(seg1, digest, EFS_HASH_SIZE);
        seg1[32] = 0; seg1[34] = 0x10; memcpy(seg1 + 36, "EFSSEG1", 8);
        assert(efs_frag_record_verify(seg1, sizeof(seg1), &id, got, NULL) == EFS_ERR_FRAG_FORMAT);
    }

    /* Incomplete identities cannot be encoded (never an unbound file). */
    struct efs_frag_ident z = id; z.generation = 0;
    assert(efs_frag_record_encode(&z, digest, rec) == EFS_ERR_INVAL);
    z = id; z.object = 0;
    assert(efs_frag_record_encode(&z, digest, rec) == EFS_ERR_INVAL);
    z = id; z.payload_len = 0;
    assert(efs_frag_record_encode(&z, digest, rec) == EFS_ERR_INVAL);
    z = id; z.fragment_index = EFS_NUM_FRAGMENTS;
    assert(efs_frag_record_encode(&z, digest, rec) == EFS_ERR_INVAL);
}

int main(void)
{
    check_kind(EFS_FRAG_KIND_CHUNK);
    check_kind(EFS_FRAG_KIND_SEGMENT);
    struct efs_frag_ident seg = base(EFS_FRAG_KIND_SEGMENT);
    uint8_t d[EFS_HASH_SIZE] = {1}, rec[EFS_FRAG_RECORD_BYTES];
    seg.chunk_index = 1; /* segments have no chunk index */
    assert(efs_frag_record_encode(&seg, d, rec) == EFS_ERR_INVAL);
    puts("frag_record: encode/verify, identity fields, payload/record corruption, unknown format PASS");
    return 0;
}
