# K38 REPORT-stage stall in `test_d4_plateau`: a swept CAS hint (Task DE)

Task DE, Nov 4 to 5 2026, node9900 and fstor007. Purpose: trace and fix mechanism B of [Task CV](k38-d4-plateau-stale-eio-20261103.md), the `pub-stale why=image-base` loop that ends in `publish-stalled` and `EIO`. Linked row: K38 (both tables).

**Verdict: PASS for the defect, K38 stays OPEN.** The cause is proven and fixed in `619eceb30`, with a deterministic regression that fails on the old classifier. `d4_plateau` stalls: **1 of 20 before, 0 of 20 after**. That count alone is weak: no run of the after arm met the trigger (0 `image-base` lines), so the recovery itself is proven by the test and the code path, not yet on a real host. K38 stays OPEN, blocks alpha: yes, until the user has a clean real-host run. Gate: `TOTAL 588 FAIL 0 NOTRUN 3 STATUS INCOMPLETE (684s) BUILD 13s`.

## Mechanism

K88 added a per-chunk CAS hint (`g_cbase` in `src/client/write.c`) for chunks with no dcache slot: the image the host last committed, keyed by FileID, used as the base of the next `base=0` record. Its comment said a truncate changes the FileID. It does not: the authority lines of both failing runs show `want=4` and `want=12` content epochs at `have=1` FileID generation.

1. The client commits an image of a chunk; `cbase_note` remembers it as the hint.
2. A truncate keeps the FileID and its sweep removes the chunk's row.
3. A later REPORT of the file loses to the truncate (`why=authority`), and the replay of that chunk finds no row, so its slot correctly drops to base 0.
4. The record rebuild reads base 0 from the slot, then `cbase_apply` replaces it with the hint. Only `cbase_apply` can do this: `putid_fill` and `dcache_object_of` set `base_gen` for spans only, and this record is a full image (`span=0+0`).
5. `host_pub_pack_inner` refuses (`why=image-base have=0`). This is correct: without `CAPTURED_EPOCH` the host cannot know the image holds none of the swept bytes.
6. The repull proves the row is gone, but nothing touches the hint. Every round sends the identical record, the unit's other chunks go with it, and after eight unchanged rounds the client logs `publish-stalled` and the application gets `EIO`.

The record should carry base 0, the swept chunk's real state, which is what the slot already says. It should not carry `CAPTURED_EPOCH`: there was no captured view to claim. The server is right and is unchanged.

The before arm caught the same sequence (run 20, ino 394501): `06:17:23.018 why=authority ci=0 ... want=4 have=1`, then eight `why=image-base ci=2 rec_base=45b62c66... flags=4 have=0` from `06:17:26.175` to `06:17:26.268`, then `publish-stalled ... ci=2 ... cycles=8 base=0 ... pub_flags=4`. The slot says `base=0` while the wire says `45b62c66...`, as in Task CV's run (`90938c...`).

## Fix

`stale_repull_replay` calls the new `cbase_reconcile` after a successful pull: it forgets the hint only if it is the exact base the sent record used and the pull contradicts it. A failed pull proves nothing and a newer noted commit wins. The next rebuild keeps base 0 and publishes. The false sentence in the K88 comment is corrected.

W123 `84b2ae60c` on `devel-w123-recovery` **is this fix**, written for the same case. Its source hunk was ported by hand, without W123's docs, graph or sanitizer script. The test is new: it models the rounds rather than one classifier call.

## Results

| Arm | Tree | Runs | `publish-stalled` runs | Runs with `image-base` | PUT-stage `EIO` runs | `rc != 0` |
| --- | --- | --- | --- | --- | --- | --- |
| before | `42bc2f046` | 20 | 1 (run 20) | 1 | 0 | 1 |
| after | `619eceb30` | 20 | 0 | 0 | 0 | 0 |

Regression, `tests/test_stale_recovery.py` (fast suite, 0.19 s on node9900, 0.2 s in the gate): with a hint on a swept chunk, in both forms of a sweep (no row, row at generation 0), the rebuilt record has base 0 on round 2. With the old classifier (`STALE_CLASSIFIER_SOURCE=<old write.c>`) it aborts at `swept_rounds(ds,no_row)==2`, having sent eight identical rounds. Controls: a failed pull keeps the hint, a row still at the hint keeps it, a newer noted commit survives. `tests/budgets.tsv` row: 30 to 5 s.

Gate on `ff1bb8b7a` (fix plus budget row), `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`, fstor007: `TOTAL 588 FAIL 0 NOTRUN 3 STATUS INCOMPLETE (684s) BUILD 13s`, no OVERBUDGET. NOTRUN: `test_cluster_stop` (no cluster scripts), `test_rdma_memcheck` (no Memcheck header), `test_qualify_rdma_live` (no RDMA hardware).

TSan, fstor007, same tree: `EFS_SAN_ONLY="data_commit d4_progress report_snapshot_locks put_ticket_batch receipt_table" make tsan-check`: `VERDICT: PASS (4 run, 0 failed, 123s)`. `report_snapshot_locks` is not in the TSan list, so four entries ran.

## Environment and build

- Loops: node9900, AMD EPYC 9474F (192 CPUs), XFS `/tmp`, `git archive` trees, plain `make` plus `make tests/faults/efsd`, gcc `-O3`, libfuse from `/data1/efs/deps/libfuse`, `build=unknown` (no git metadata). `tests/test_d4_plateau.py` defaults: three `efsd` of the `EFS_FAULTS` build, two `efs-fuse` mounts, 2 GiB quota, GC on, 75 s load, 8 threads, 128 files per writer, 150 s drain, direct I/O, `transport auto` (RDMA on loopback on this host, per Task CV). Settings of Task CV's `fixkeep` arm: `EFS_PUT_TICKET_DBG=1 EFS_D4_KEEP=1`.
- Hashes, before: `efsd` `1d8ebeef…`, `efs-fuse` `af16f74c…`, `tests/faults/efsd` `d62ed643…`. After: `91e89895…`, `83d5a27c…`, `2ac9f194…`.
- The arms ran serially within themselves and overlapped each other in one host, so both saw the same background load (load average up to about 15 of 192 CPUs). Other sessions' `screen`s were present. Only private ports; no live cluster process was touched.
- Gate and TSan: fstor007 (96 CPUs), libfuse through a private shim directory.

## Reproduction

```sh
export EFS_FUSE_PREFIX=/data1/efs/deps/libfuse PKG_CONFIG_PATH=$EFS_FUSE_PREFIX/lib64/pkgconfig LD_LIBRARY_PATH=$EFS_FUSE_PREFIX/lib64
make -j32 && make -j32 tests/faults/efsd
python3 tests/test_stale_recovery.py                                   # deterministic
git show 42bc2f046:src/client/write.c > /tmp/old-write.c
STALE_CLASSIFIER_SOURCE=/tmp/old-write.c python3 tests/test_stale_recovery.py   # must abort at swept_rounds
for i in $(seq 1 20); do export TMPDIR=/tmp/de/run-$i; mkdir -p $TMPDIR
  EFS_PUT_TICKET_DBG=1 EFS_D4_KEEP=1 python3 tests/test_d4_plateau.py > run-$i.log 2>&1; echo "run=$i rc=$?"
done   # in a detached screen, about 100 s per run; delete each $TMPDIR afterwards
# a stall: grep publish-stalled in d/mnt-l.log and 'why=image-base' in d/n*.log of the kept tree
EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh                      # the gate, from an archive of the commit
```

## Limits and remaining gates

- **The after arm did not exercise the fix on a real host.** With the fix, one occurrence of the trigger should show exactly one `image-base` line and then publish; the arm has none. Unfixed, the rate is 2 of 40 runs (Task CV's 1 of 20 and this one), so 0 of 20 has about a 36 % chance even with no fix. The recovery rests on the deterministic test, which runs the production classifier and hint table against a modelled host.
- No real-daemon deterministic reproducer. Six one-off runs did not reach the trigger on the before tree: two with one mount (write, `fsync`, `ftruncate`, wait, rewrite; 3.5 MiB and 15 MiB files), four with two mounts, the second truncating under the first's open file (15 MiB and 448 KiB, waits 0 and 1.5 s). The two-mount form reached `why=authority` on every iteration and then recovered without `image-base`. The exact preceding state in the load was not determined. The two-mount script is `live.py` in the artifact directory.
- A slot-less writer (`efs-bench`) whose hint is retired while the row moved to another client's image sends base 0 and loses as before K88; `report_rec_stuck` still bounds it to a writeback error. Not observed.
- Owed for K38: a clean user real-host run, and the orcd-mini `ior hard` and shared-chunk reruns from the earlier rows.
- `docs/status/README.md` needs no change: its Current table verdicts did not change (K38 stays OPEN).

Raw output is outside Git: `~/efs-artifacts/taskDE-20261104/` (shared `erbmi1` home on node9900 and fstor007), `SHA256SUMS` sha256 `e3a398bd4a8c49fba2a8e281d0a92e6612301abf1b0dd4d3737dd2ee14ba0364`. It holds both arms' summaries and run logs, the kept failing run, the gate summary and timings, the TSan output, and the loop scripts.
