# K33 — placement spread for more than four nodes (Oct 24, 2026)

**Verdict: fix and unit tests done; distribution PASS (tables below); the
code gate (`make test-all` on amd) is PENDING — amd and every other
bare-metal test host were unreachable from the Mac when the fix was ready
(no ARP answer for 10.0.0.201/203/205/207/209). Branch
`claude/devel-placement-spread-b6a3c4` on `merge-wave2` (`3e01ba95c`), not
pushed. Live multi-host, RDMA and orcd-mini gates NOTRUN (parent session).
Needs a fresh format for new placement (no compatibility; recorded
placement of existing chunks still reads).**

Purpose: close the open finding of
[K33 increment 2](k33-w157-autoid-discovery-20261024.md) ("placement
collapses for power-of-two member counts"), an alpha 1.0 item
([K33](alpha-1.0-2plus1-checklist.md) / [W157](work-queue.md)): 2+1 on
more than four nodes must spread new data over every member, 3 to 16.

## Cause

`place_wide` started each stripe at `fnv1a(ino) % n`. FNV-1a multiplies by
an odd prime after each XOR, so output bit *j* depends only on bits 0..*j*
of each input byte. Inode numbers of one shard are `s + k·2^shard_bits`
(`efs_export_shard_of`), so for `shard_bits` 12 the low byte is constant and
byte 1 changes only in its high nibble: for `n` a power of two (and for
`n` = 6, 12, 14 through the low bits of `n`) a run of new files all got one
start rank. Long runs eventually balance as higher bytes change; short runs
(the 24 files of the growth test) do not. Packed segment ids
(`now_ns << 8 | self`, `packer.c`) collapsed the same way on a clock with
microsecond resolution.

## What changed (commit `95a0e678a`)

- `src/common/placement.c`: `place_mix` = murmur3 `fmix64` (every output bit
  depends on every input bit); `place_range` reduces with a 64×64→128
  multiply-shift on the high bits, no modulo (each rank has ⌊2^64/n⌋ or
  ⌈2^64/n⌉ preimages, relative bias < n/2^64). Stripe start =
  `place_range(place_mix(ino), n) + chunk_index (mod n)`: uniform across
  files, and one file's chunks still walk every rank exactly evenly (stride
  `n/3` unchanged). Packed segments (D38, `efs_pack_destinations`) and meta
  table pages (`place_consecutive`) use the same hash. New public helper
  `efs_place_index(a, b, n)`.
- Spares (`efs_place_spare`, used by `reroute_untried_fragments` in
  `src/client/write.c`): a fragment whose stripe member is down goes to a
  live member outside the stripe chosen by hash of (ino, chunk, slot) over
  the candidates in ascending id order. Before, the first live member in the
  membership list outside the stripe took every displaced fragment.
- Capacity-aware skips: none exist. A full node answers `EFS_ERR_QUOTA`;
  with one refusal the write is acknowledged degraded, with two it is
  refused (`put_fragments_parallel_once`). No placement decision to fix.
- New writes only: readers use the `fragment_nodes` recorded in the chunk
  table (`read.c` "the chunk table is the source of truth"); packed
  segments record theirs in the pack object. Recomputation is only the
  fallback for a chunk with no row.
- `tests/d39_model.py` `place_wide` mirrors the new start; the model test
  still passes (118 passed, 0 failed, 19 negative controls, local run).
- No KV kind, Raft tag or wire message type added.

## Tests

- `tests/test_placement.c` (in `make test`):
  - `spread_fair`: for every N = 3..16 and seven patterns — one shard with
    step 4096 (two chunks per file), one shard with step 8, 64 shards
    round-robin, sequential, a `0x1234 << 40` prefix with step 4096, packed
    segment ids on a microsecond and a nanosecond clock — each node holds
    within **±10 %** of the mean over 8192 items, and **no window of 64
    consecutive items leaves any member without a fragment**; a 64·N-chunk
    file puts exactly 192 fragments on every node.
  - `spread_eight_member_regression`: eight members, 24 files × 2 chunks,
    inodes `4096k + 7`: every node gets at least half the mean (9 of 144).
  - `spread_spares`: for N = 4..16, node 1 down, 16384 chunks: the spare is
    never the down node or a stripe member, does not depend on list order,
    at least N − 3 members receive spares and none takes more than 1.75×
    the mean of the receivers; edge cases (no candidate, bad slot, range).
  - Negative control: the same test linked with the old FNV start fails
    (548 `spread` failures, including `8 members node 2 got 0 of 144`).
- `tests/test_k33_growth.py` (in `make test-long`): every added node
  (4, 6, 7, 8) holds fragments after the 24 new files, not only their sum.
- `tests/test_put_retry_budget.py`: stub follows the new
  `reroute_untried_fragments(ino, chunk, nodes)` signature.

## Distribution, before and after

Reproduce (deterministic; the tool uses only `efs_place_fragments` and
`efs_pack_destinations`, so it links against either placement.c):

```sh
cc -O2 -Iinclude tests/placement_spread_table.c src/common/placement.c -o /tmp/spread
git show 3e01ba95c:src/common/placement.c > /tmp/placement_before.c
cc -O2 -DBEFORE -Iinclude tests/placement_spread_table.c /tmp/placement_before.c -o /tmp/spread_before
/tmp/spread_before > before.md; /tmp/spread > after.md
```

Outputs (Mac, `95a0e678a`): `before.md` sha256 `a9163b44…2936`,
`after.md` sha256 `a01cdbfd…a215`; both reproduce bit for bit.

### The finding's case: 8 members, 24 files × 2 chunks, inodes 4096k + 7

Before (rank 2 idle; the daemon test, with another shard offset, left
members 3 and 4 idle):

| n1 | n2 | n3 | n4 | n5 | n6 | n7 | n8 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 9 | 0 | 15 | 24 | 24 | 24 | 24 | 24 |

After:

| n1 | n2 | n3 | n4 | n5 | n6 | n7 | n8 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 20 | 19 | 18 | 17 | 15 | 16 | 19 | 20 |

### Short runs: 16-item windows (of 512) leaving some member idle

Before:

| N | shard step 4096, 2 chunks | shard step 8, 1 chunk | 64 shards, 1 chunk | sequential, 1 chunk | prefix<<40, step 4096 | packed seg, us clock | packed seg, ns clock |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 4 | 0 | 257 | 0 | 0 | 512 | 0 | 0 |
| 5 | 0 | 0 | 0 | 27 | 15 | 0 | 0 |
| 6 | 0 | 257 | 0 | 0 | 512 | 0 | 0 |
| 7 | 0 | 0 | 0 | 2 | 0 | 0 | 1 |
| 8 | 129 | 512 | 0 | 0 | 512 | 0 | 0 |
| 9 | 0 | 3 | 34 | 0 | 7 | 3 | 0 |
| 10 | 0 | 136 | 42 | 61 | 196 | 25 | 13 |
| 11 | 0 | 18 | 96 | 60 | 18 | 30 | 38 |
| 12 | 511 | 512 | 0 | 0 | 512 | 400 | 0 |
| 13 | 0 | 98 | 80 | 133 | 201 | 87 | 83 |
| 14 | 134 | 512 | 0 | 16 | 512 | 183 | 76 |
| 15 | 0 | 157 | 90 | 240 | 196 | 58 | 71 |
| 16 | 512 | 512 | 0 | 0 | 512 | 378 | 23 |

After:

| N | shard step 4096, 2 chunks | shard step 8, 1 chunk | 64 shards, 1 chunk | sequential, 1 chunk | prefix<<40, step 4096 | packed seg, us clock | packed seg, ns clock |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 4 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 6 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 7 | 0 | 0 | 0 | 0 | 1 | 0 | 1 |
| 8 | 0 | 4 | 2 | 2 | 4 | 1 | 4 |
| 9 | 0 | 6 | 3 | 1 | 3 | 3 | 5 |
| 10 | 0 | 15 | 20 | 9 | 8 | 15 | 16 |
| 11 | 0 | 24 | 30 | 33 | 23 | 39 | 35 |
| 12 | 0 | 22 | 25 | 22 | 19 | 18 | 20 |
| 13 | 0 | 72 | 86 | 92 | 86 | 75 | 70 |
| 14 | 1 | 116 | 117 | 110 | 93 | 121 | 84 |
| 15 | 0 | 55 | 93 | 74 | 63 | 63 | 59 |
| 16 | 5 | 183 | 197 | 204 | 190 | 202 | 190 |

Before, the collapse is structural (512 of 512 windows for N = 8, 12, 16
with step 4096 or a high prefix). After, what remains is chance: 16
single-chunk items give 48 fragments, and for N = 16 a member misses all
of them with probability (13/16)^16 ≈ 3.6 %, so about 37 % of windows
expected (measured 183–204 of 512, 36–40 %).

### 64-item windows (of 128) leaving some member idle

Before:

| N | shard step 4096, 2 chunks | shard step 8, 1 chunk | 64 shards, 1 chunk | sequential, 1 chunk | prefix<<40, step 4096 | packed seg, us clock | packed seg, ns clock |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 4 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 5 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 6 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 7 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 8 | 0 | 128 | 0 | 0 | 0 | 0 | 0 |
| 9 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 10 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 11 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 12 | 0 | 128 | 0 | 0 | 0 | 0 | 0 |
| 13 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 14 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| 15 | 0 | 12 | 3 | 13 | 8 | 0 | 0 |
| 16 | 0 | 128 | 0 | 0 | 128 | 0 | 0 |

After: 0 in every cell (the unit test asserts it).

### Whole run: lightest and heaviest node as share of the mean (8192 items)

Over long runs the old hash balanced (higher bytes change) except for
spares. Before, spares only (all other columns 0.96–1.04):

| N | spare, node 1 down (before) |
|---:|---|
| 3 | - |
| 4 | 1.00-1.00 |
| 5 | 0.00-1.33 [1] |
| 6 | 0.00-5.00 [4] |
| 7 | 0.00-6.00 [5] |
| 8 | 0.00-7.00 [6] |
| 9 | 0.00-8.00 [7] |
| 10 | 0.00-9.00 [8] |
| 11 | 0.00-10.00 [9] |
| 12 | 0.00-11.00 [10] |
| 13 | 0.00-12.00 [11] |
| 14 | 0.00-13.00 [12] |
| 15 | 0.00-14.00 [13] |
| 16 | 0.00-15.00 [14] |

After:

| N | shard step 4096, 2 chunks | shard step 8, 1 chunk | 64 shards, 1 chunk | sequential, 1 chunk | prefix<<40, step 4096 | 16 files x 512 chunks | packed seg, us clock | packed seg, ns clock | spare, node 1 down |
|---:|---|---|---|---|---|---|---|---|---|
| 3 | 1.00-1.00 | 1.00-1.00 | 1.00-1.00 | 1.00-1.00 | 1.00-1.00 | 1.00-1.00 | 1.00-1.00 | 1.00-1.00 | - |
| 4 | 1.00-1.00 | 0.99-1.01 | 0.99-1.01 | 1.00-1.01 | 1.00-1.00 | 1.00-1.00 | 0.99-1.01 | 0.99-1.01 | 0.99-1.01 |
| 5 | 1.00-1.00 | 0.99-1.01 | 0.99-1.01 | 0.99-1.01 | 0.99-1.01 | 1.00-1.00 | 0.99-1.01 | 0.99-1.01 | 0.66-1.34 |
| 6 | 1.00-1.00 | 1.00-1.00 | 0.99-1.01 | 0.99-1.01 | 0.99-1.01 | 1.00-1.00 | 0.99-1.01 | 1.00-1.00 | 0.00-1.68 [2] |
| 7 | 0.99-1.00 | 0.99-1.01 | 0.98-1.03 | 0.98-1.01 | 0.98-1.02 | 1.00-1.00 | 0.99-1.01 | 0.98-1.02 | 0.51-1.51 |
| 8 | 1.00-1.00 | 0.99-1.01 | 0.95-1.04 | 0.97-1.02 | 0.98-1.02 | 1.00-1.00 | 0.99-1.01 | 0.98-1.02 | 0.46-1.42 |
| 9 | 0.99-1.01 | 0.99-1.02 | 0.98-1.02 | 0.98-1.02 | 0.99-1.01 | 1.00-1.00 | 0.98-1.01 | 0.98-1.01 | 0.00-1.36 [2] |
| 10 | 1.00-1.00 | 0.98-1.02 | 0.98-1.02 | 0.98-1.04 | 0.98-1.03 | 1.00-1.00 | 0.98-1.02 | 0.98-1.01 | 0.44-1.33 |
| 11 | 0.99-1.01 | 0.97-1.03 | 0.97-1.03 | 0.97-1.03 | 0.96-1.03 | 1.00-1.00 | 0.96-1.03 | 0.97-1.03 | 0.39-1.31 |
| 12 | 0.99-1.01 | 0.99-1.01 | 0.99-1.02 | 0.98-1.01 | 0.98-1.01 | 1.00-1.00 | 0.98-1.02 | 0.97-1.03 | 0.00-1.35 [2] |
| 13 | 0.99-1.01 | 0.97-1.03 | 0.96-1.05 | 0.95-1.04 | 0.98-1.02 | 1.00-1.00 | 0.97-1.04 | 0.97-1.03 | 0.39-1.32 |
| 14 | 0.99-1.01 | 0.97-1.05 | 0.97-1.07 | 0.96-1.04 | 0.96-1.03 | 1.00-1.00 | 0.97-1.04 | 0.96-1.04 | 0.37-1.29 |
| 15 | 0.99-1.00 | 0.98-1.02 | 0.99-1.02 | 0.96-1.02 | 0.99-1.01 | 1.00-1.00 | 0.98-1.02 | 0.97-1.02 | 0.00-1.28 [2] |
| 16 | 0.98-1.02 | 0.96-1.04 | 0.97-1.03 | 0.98-1.02 | 0.95-1.03 | 1.00-1.00 | 0.96-1.03 | 0.96-1.04 | 0.36-1.32 |

Spare zeros after the fix are structural: with N divisible by 3 and stride
N/3 the down node's stripe-mates are fixed (N = 6: node 1 is always with 3
and 5) and can never be its spare.

## Code gate

PENDING: `make test-all` on amd (private tree under `$HOME`, `git archive`
of the tested commit, base `3e01ba95c` run for comparison) was not run:
amd (10.0.0.205), intel1, intel2, xorinox and nuc did not answer on the
LAN (ARP incomplete, ssh timeouts) on Oct 24; the efs1–3 VMs run the
user's own cluster and are not test hosts. Run locally on the Mac instead
(EFS itself does not build on macOS): `tests/test_placement.c` with
`src/common/placement.c` (OK, 0.1 s) and `python3 tests/test_d39_model.py`
(118 passed, 0 failed).

## NOTRUN

- `make test-all` on amd, fix and base (host unreachable; see above).
- Live multi-host addition with client load, RDMA, orcd-mini (parent
  session).
