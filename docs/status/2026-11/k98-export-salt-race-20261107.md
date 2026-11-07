# K98: `host_export_salt` lazy cache race (Task DS)

Verdict: **PASS** (K98 FIXED, blocks alpha: no). Linked item: K98 in
[the checklist](../alpha-1.0-2plus1-checklist.md). Purpose: fix the intermittent
server TSan race that Task DR's before run reported next to K97
([K97](k97-resolve-ms-race-20261106.md), Limitations).

## Race and fix

`host_export_salt()` (`src/server/raft_host.c`) caches the per-export MKDIR
scatter salt in `struct efs_raft_host` (`export_salt`, `export_salt_valid`)
after the first KV read. The salt is written once at raft-mkfs. The check and
the fill were plain loads and stores outside `h->mu`. Under
`tests/test_k96_status_slots_live.py` the 24 `efs-bench` writers create their
files at once, so several MKDIR handler threads on one `efsd` reach the empty
cache together. TSan reported a write-write race on `:7486` (`export_salt`) and
`:7487` (`export_salt_valid`), and once the read of the flag at `:7469`, all via
`server_raft_host_mkdir` (`:20670`).

Fix (`d700c0ee1`): the K83/K97 idiom. The flag is acquire-loaded and
release-stored, and the salt is a relaxed atomic load and store before it. Two
first threads may both read the KV. That read is idempotent and gives the same
immutable value. The salt is not known at daemon start (mkfs may come later), so
an init at start would not do. No timeout, budget or default changed.

## Tested

- Base devel `7d5e56026`, which is also the before tree. The tested source is
  `d700c0ee1`, the fix alone on that base. Both trees were shipped with
  `git archive`, so the build id reads `unknown`.
- Host node9900 (192 CPUs, gcc 11.5.0, libtsan), private libfuse 3.16.2
  (`EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`, `PKG_CONFIG_PATH` and
  `LD_LIBRARY_PATH` to its `lib64`). TCP loopback, buffered I/O, harness-chosen
  ports, `TMPDIR=/tmp/efs-taskDS-tmp`. Three production `efsd`, 24 library
  writers, one round per run (`K96_ROUNDS=1`, as the TSan list runs it).
- Commands, from each tree's root:
  - The full list, once per tree, keeping the build:
    `EFS_SAN_LOGDIR=<dir> bash tests/sanitizer_check.sh tsan`.
  - Repeats on that build:
    `EFS_SAN_BUILD=<dir>/build EFS_SAN_LOGDIR=<rep> EFS_SAN_ONLY=k96_status_slots_live bash tests/sanitizer_check.sh tsan`.
    Each rep counts `WARNING: ThreadSanitizer` lines and `host_export_salt`
    frames in its `.tsan.*` files.
  - `make -j32 test` on the after tree.
  - Cleanup: remove both trees and `TMPDIR`.

## Results

| Tree | Runs of `k96_status_slots_live` | Runs reporting this race | Other races |
| --- | --- | --- | --- |
| before `7d5e56026` | 60 reps (20 + 40), plus 1 in the full list | 8 of 60 reps (2 reports each); 0 in the full list | 1 rep: `frag_loc_note` (below) |
| after `d700c0ee1` | 60 reps, plus 1 in the full list | **0 of 61** | 1 rep: `frag_loc_note` (below) |

Before, the race appeared in 8 of 60 reps. At that rate, the chance of 60 clean
after-reps without a fix is about 0.02% ((52/60)^60).

- Full TSan list: before `VERDICT: FAIL (26 run, 1 failed)`. The failure was
  `d4_progress`, test rc=1 with no TSan report: one packer `read_errors` under
  TSan load. That is not this row and was not traced here. After:
  `VERDICT: PASS (26 run, 0 failed, 604s)`.
- `make test` on the after tree: rc 0. Locally, `python3 tests/csrc.py` and
  `python3 scripts/check_include_boundary.py` pass.

## Not this row: `frag_loc_note` race (no K row yet)

In 1 of 60 reps on each tree, TSan reported writes racing in `frag_loc_note`
(`src/server/store.c:2766`-`2772`, the global `frag_loc_cache`). Two
`writer_thread`s (`server_write_fragment_with_sum_sync` <-
`server_fragment_path`) filled the same hash slot at once. The fill drops
`valid` first and raises it last with a release store, which guards against a
racing *reader*. It does not stop a second *writer*: interleaved fills can leave
a mixed entry with `valid = 1`, which a reader would trust. This is a real
defect, separate from K98. It is left for a follow-up with its own K row.

## Limitations

- Only node9900 and only the K96 live entry was repeated. The other 25 entries
  of the TSan list ran once per tree.
- Lock-order inversions are out of the gate's scope (`detect_deadlocks=0`).
- Raw output: node9900 `~/efs-artifacts/taskDS-20261107/` (`node9900-tsan.tar.gz`
  holds every rep's gate log and TSan reports, the full-list logs and the
  `make test` log). `SHA256SUMS` there has sha256
  `d3931870fc96393097f0b5541eb2ff5990047ad0c195521a90d443d592ab797c`, and the
  tarball's sha256 is
  `34283620f1ed1a4e23182be3ca385952be3a6c3c455dee42825e89e52b2c5e8f`.
