# K99: `frag_loc_cache` fill race (follow-up of Task DS)

Verdict: **PASS** (K99 FIXED, blocks alpha: no). Linked item: K99 in
[the checklist](../alpha-1.0-2plus1-checklist.md). Purpose: fix the intermittent
server TSan race that Task DS found next to K98
([K98](k98-export-salt-race-20261107.md), "Not this row").

## Race

`frag_loc_cache` (`src/server/store.c`) is a process-wide, unlocked,
direct-mapped cache that maps a fragment (export, inode, chunk, fragment, gen)
to the storage root that holds it. `server_fragment_path` fills it on every PUT
(`frag_loc_note`), and a multi-root GET reads it in `read_fragment_impl`. The
fill was plain stores with a release store of `valid` last. Two `writer_thread`s
(`writer.c` `run_job` -> `server_write_fragment_with_sum_sync` ->
`server_fragment_path` -> `frag_loc_note`) filling one slot at once were
unsynchronised writes, and TSan reported them on `store.c:2766`-`2772`. The
reader's plain loads, and its plain `valid = 0` after ENOENT, race with a fill
in the same way. Each node in `k96_status_slots_live` has one storage root, so
the reader branch (`n != 1`) does not run there and only the writer-writer pair
was reported.

## Can a mixed entry mislead a reader? No (verified in the code)

Interleaved fills can leave a slot with `valid = 1` and fields from two fills.
The reader treats the entry only as a hint, and three checks hold:

1. Key recheck. The reader compares export, inode, chunk, fragment and gen with
   the request. A mixed key that is not the requested one misses, and the reader
   probes every root.
2. A wrong root is only a slower path. The fragment file name contains the gen
   (`fragment_path_at`). A PUT reuses the root that already holds the fragment
   (`pick_write_path`, `writer.c:157`, fed by `server_find_fragment_root`). A
   wrong root therefore gets ENOENT, the reader clears `valid` and probes all
   roots.
3. Verification after the open. `read_fragment_verified` checks the I25
   record's identity (export, inode, chunk, fragment, generation) and, for data
   fragments, the payload digest. Anything else is refused and never served.

So the defect was a C data race (undefined behaviour, and noise that can hide
real races in the TSan gate), not wrong data. The existing contract comment
already said a torn entry is harmless.

## Fix

`b3002b5a4`: every access to a `frag_loc_cache` field uses a relaxed
`__atomic` load or store through `FRAG_LOC_SET`/`FRAG_LOC_GET`. That covers the
eight fill stores, the reader's key loads and its `valid = 0`. The reader now
loads `root` once, so the root it bounds-checks is the root it opens. A seqlock
or CAS-claimed slot was not needed, because the reader revalidates (above). The
hot path stays lock-free: on x86-64 a relaxed atomic is a plain move. No
timeout, budget or default changed.

## Tested

- Before tree: devel `7d5e56026`. After tree: `b3002b5a4`, the fix alone on
  that base. Neither tree has K98 (`d700c0ee1`), so the K98 `host_export_salt`
  reports appear on both sides and are counted apart. Both trees were shipped
  with `git archive`, so the build id reads `unknown`.
- Host node9900 (192 CPUs, gcc 11.5.0, libtsan), private libfuse 3.16.2
  (`EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`, `PKG_CONFIG_PATH` and
  `LD_LIBRARY_PATH` to its `lib64`). TCP loopback, buffered I/O, harness-chosen
  ports, `TMPDIR=/tmp/efs-taskK99-tmp/{before,after}`. Three production `efsd`
  with one storage root each, 24 `efs-bench` copies of 4 writers, one round per
  run (`K96_ROUNDS=1`, as the TSan list runs it). The two series ran at the
  same time on the host, so both saw the same load.
- Command (the script is on the K99 branch, not on devel; it was copied to the
  host beside the before tree), from each tree's root:
  `bash scripts/evidence/k99/k99_tsan_reps.sh <tree> <out> 110`. It builds a
  TSan tree once (`EFS_SAN_LOGDIR=<out>/build0 EFS_SAN_ONLY=k96_status_slots_live
  bash tests/sanitizer_check.sh tsan`), then runs 110 reps on that build with
  `EFS_SAN_BUILD`. It writes `reps.tsv` with each rep's gate rc, its
  `WARNING: ThreadSanitizer` count, and the number of reports naming
  `frag_loc` and `host_export_salt`.
- `make -j32 test` on the after tree.
- Cleanup: remove `/tmp/efs-taskK99` and `/tmp/efs-taskK99-tmp`.

## Results

| Tree | Reps | Reps reporting this race (reports) | Other TSan reports | Other failures |
| --- | --- | --- | --- | --- |
| before `7d5e56026` | 110 (+1 build run) | **3 of 110** (r004 14, r021 7, r100 7) | `host_export_salt` (K98) in 16 reps | none |
| after `b3002b5a4` | 110 (+1 build run) | **0 of 110** | `host_export_salt` (K98) in 28 reps | 1 rep, no TSan report (below) |

Both build runs passed. The counts are close to the earlier rate (1 of 60 on
each of Task DS's trees). The clean after series alone is weak statistically:
with no fix, 110 clean reps at 3 in 110 would happen about 5% of the time
((107/110)^110). The fix rests on construction. After it, every access to the
cache is atomic, and TSan does not report races between atomic accesses. The
reps confirm the reports are gone and that nothing new appeared.

- `make -j32 test` on the after tree: rc 0. Locally, `python3 scripts/check_k_register.py`,
  `python3 scripts/check_doc_links.py`, `python3 docs/check-architecture.py` and
  `python3 tests/csrc.py` pass.

## Not this row: session revoke at unmount timed out (no K row yet)

After rep r009 failed with no TSan report. Three of its 24 `efs-bench` copies
exited rc=1 after 21.6 s with
`efs: session revoked at unmount: ... rc=-6 ms=~15700 ...` and
`unmount not clean: flush rc=0 settle rc=0 revoke rc=-6`. The node logs show
telemetry overflow at the time. That is 1 of 220 runs, under TSan load, with
no K98 fix in the tree. This change cannot reach that path: in this test the
cache only receives stores, and the cache selects a read root. It was not
traced here and is left for a follow-up.

## Limitations

- Only node9900, and only the K96 live entry. The full TSan list was not rerun
  for this one-function change.
- The multi-root reader branch is not exercised by a TSan entry. It was checked
  by reading the code (above), not by a run.
- Lock-order inversions are out of the gate's scope (`detect_deadlocks=0`).
- The branch head (devel + K98 + K99) was not itself run under TSan. Each fix
  was tested alone on devel, and they touch different files (`raft_host.c`,
  `store.c`).
- Numbering: K99 assumes K98 (branch `claude/keen-wu-2f4bf0`). The K99 branch
  merges that branch in after the tested code commit, so the register stays
  contiguous.
- Raw output: node9900 `~/efs-artifacts/taskK99-20261107/`
  (`node9900-tsan.tar.gz` holds both `reps.tsv` files, every rep's gate log and
  TSan reports, the r009 node logs and the `make test` log). `SHA256SUMS` there has sha256
  `05ede569eb81848f19084252e5af3c648008971fbe55c747f191c2639c3a087b`, and the
  tarball's sha256 is
  `167a33fb122d5d50bdb2bc45ca3ce23c120ea7e5329e65ed12fb322ad8da037e`.
