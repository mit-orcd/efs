# F1 — memory and concurrency gates on the alpha 1.0 candidate (Oct 25)

**Purpose.** Release check F1 ([checklist](alpha-1.0-2plus1-checklist.md),
section F) needs the memory and concurrency gates to have explicit
PASS/FAIL/NOTRUN results on the candidate, after the Oct 23–25 changes (K32
batching, K33 membership and discovery, K34, K35, K36 recovery thread, K37
segment rebuild, W126 KV peek lock, W155 receipts, D31, E2 parallel revoke,
A7, READ_VERIFY). The gates last ran before those changes. This page runs
every gate, wires the new unit binaries into them, and fixes what they found.

**Verdict.** On xorinox at `TBD_COMMIT`:

| Gate | Command | Result |
| --- | --- | --- |
| Leak (valgrind memcheck, K26) | `make leak-check` | **TBD** |
| ThreadSanitizer | `make tsan-check` | **TBD** |
| AddressSanitizer + UBSan + LeakSanitizer | `make asan-check` | **TBD** |
| Daemon RSS plateau (20 min D4 load) | `tests/test_d4_progress.py` + `scripts/rss_plateau.py` | **TBD** |
| Code gate | `make test-all` | **TBD** |
| RDMA memcheck (`tests/test_rdma_memcheck.py`) | | NOTRUN: no RDMA device on xorinox (parent session) |

Branch `claude/devel-f1-gates-46ff64` on `merge-wave7` (`0b01a2a1e`), not
pushed, not merged.

## What changed

New gates and wiring (test and build code only):

- `tests/sanitizer_check.sh`, `make tsan-check` / `make asan-check` /
  `make sanitize-check`: a copy of the tree built with the sanitizer, a
  bounded list per mode, one PASS/FAIL line per entry and a verdict, exit 1
  on any report, 3 (`NOTRUN: <mode> runtime missing`) without the runtime.
  Every sanitized process writes its reports through `log_path` to its own
  file, so a report inside efsd or efs-fuse started by a Python harness
  counts. Lists and rules: [testing.md](../how-it-works/testing.md#make-tsan-check-and-make-asan-check).
- `tests/test_sanitizer_gate.py` (in `make test`): the gate's self-test
  (racy, overflowing, UB, leaking and failing fixtures FAIL; a clean one
  PASSes; a missing runtime is NOTRUN exit 3; `.py` entries and `VAR=value`
  prefixes work).
- `tests/sanitize_env.py`: compile flags, memcheck wrapper and scaled
  timeout for the Python-driven C harnesses; `test_put_ticket_batch.py`,
  `test_w155_receipt_table.py` and `test_session_revoke_parallel.py` use it
  and now run under all three gates.
- `make leak-check` list: 16 new entries (41 in all): `kv_lsm_concurrent`,
  `membership`, `member_view`, `data_commit`, `put_ticket`, `put_orphan`,
  `put_orphan_server`, `append_receipt`, `frag_record`, `small_segment`,
  `packed_segment`, `pack_ledger_recovery`, `retained_namespace_audit`,
  and the three harnesses above. Memcheck runs with `--fair-sched=try`
  (finding 5).
- `scripts/rss_plateau.py`: the plateau verdict over a D4 run's RSS samples.
- `tests/test_d4_progress.py`: RSS bounds scale ×6 under a sanitizer build
  (finding 7); the native bound is unchanged.

## Findings

Every finding below was found by one of the gates in this session and is
fixed on the branch; the gate entry that found it is its regression test
and passes after the fix.

1. **UB in the recovery thread, `src/server/recovery.c:757` and `:802`**
   (asan gate, `test_k36_recovery.py` and `test_k36_segment_recovery.py`):
   `rv_watch_pass` and `rv_seg_watch_pass` end with
   `memmove(rv.marks + keep, rv.marks + n, 0)` when nothing is remembered;
   `rv.marks` / `rv.segs` are then NULL and `memmove(NULL, NULL, 0)` is
   undefined (UBSan `null pointer passed as argument 1`; under
   `halt_on_error` the daemon exited in the rebuild phase and the test lost
   node 3). Product fix: the move runs only when there is something to
   move. Natively glibc's memmove tolerates it, so no behaviour change.
2. **Leak at the removed-setting refusal, `src/common/log_ts.c`** (asan
   gate, `test_env_removed.py`): LeakSanitizer reported the two 8-byte
   `fopencookie` cookies of the timestamp streams as direct leaks when
   `efsd` exits 1 on a removed `EFS_*` setting (the same at every daemon
   exit). glibc keeps the cookie pointer in a form LSan does not follow.
   Product fix: the cookies are anchored in a static table and freed by a
   `close` hook if the stream is ever closed. 16 bytes per process, exit
   path only; no runtime change.
3. **Misaligned struct read, `tests/test_wire.c:472`** (asan gate, `wire`):
   the K28 hello-ack case cast the frame's payload pointer (behind the frame
   header, unaligned) to `struct efs_msg_hello_ack` (UBSan `member access
   within misaligned address`). Test fix: copy into an aligned struct, as
   the list-nodes case next to it already did. x86 tolerates the access;
   the product decoder does not do this.
4. **ASan fake stack vs AVX-512 BLAKE3** (asan gate, every hashing binary):
   with `-march=native` on xorinox (Ryzen 9 7940HS, AVX-512) gcc 14.3 emits
   64-byte-aligned vector stack accesses in `deps/blake3/blake3.c`
   (`make_output`, `compress_subtree_to_parent_node`); ASan's fake stack for
   `detect_stack_use_after_return=1` does not give that alignment and every
   binary that hashed died with `SEGV on unknown address` (19 of 47
   entries, all three efsd of every daemon test). With the fake stack off
   the same binaries pass. Not a product defect: the gate builds its ASan
   tree with a baseline ISA (`-march=x86-64-v3`, as the leak gate does for
   valgrind) and keeps use-after-return detection; `EFS_SAN_ARCH` overrides.
5. **Memcheck starved the KV writer** (leak gate, `kv_lsm_concurrent`):
   under valgrind's default thread scheduling the four reader threads of
   `test_kv_lsm_concurrent` kept the writer from one flush for more than 3
   minutes (`l0 + l1 > 0` failed at 4 s; with the test waiting for a flush,
   its 124 s alarm fired); `--fair-sched=yes` finishes the same 4 s stress
   in 17 s. Native scheduling never showed this; `view_rw` is
   writer-preferring. Gate fix: memcheck runs with `--fair-sched=try`. Test
   fix: the stress runs for its seconds and then until a flush has landed,
   instead of assuming one within the wall-clock budget.
6. **Speed ratio under instrumentation** (leak gate, `revoke_parallel`):
   the E2 harness asserts `parallel(16) * 4 < serial`; memcheck serialises
   threads (3.9 s vs 2.9 s). Test fix: the ratio is asserted natively only
   (`-DEFS_INSTRUMENTED` from `sanitize_env`); the overlap check
   (`maxflight > 1`) stays under every gate. The harness now prints its
   binary's output on failure (it was lost in `check=True`).
7. **RSS bound under ASan** (asan gate, `d4_progress`): the 60 s ASan load
   reached a 1264 MiB efs-fuse against the 1024 MiB bound where the native
   20-minute run stays near TBD MiB (shadow memory, redzones, quarantine).
   Test fix: the bounds scale ×6 under a sanitizer build only.

No data race was reported by ThreadSanitizer: TBD. No definite leak in
product code under memcheck.

## Runs

TBD_RUNS

## RSS plateau

TBD_RSS

## Environment

xorinox: AMD Ryzen 9 7940HS (16 CPUs, AVX-512), 60 GB RAM, Rocky Linux 10
(kernel 6.12.0-211), gcc 14.3.1, glibc 2.39, valgrind 3.24, libtsan/libasan
8, `kernel.yama.ptrace_scope` 0 (LeakSanitizer works), `/dev/fuse` and
`fusermount3` present. Private trees under `$HOME/efs-f1*`, `TMPDIR`
`$HOME/efs-tmp` (XFS, 800 GB free). Loopback daemons on ports the tests
choose; the host's live cluster mount (`/data1/efs/intel-mnt`, user `efs`)
and the 17xxx ports were not touched. Other sessions' runs shared the host
at times (load average up to 8).

## Reproduction

From a clean tree on a Linux host with valgrind, libtsan and libasan:

```bash
git archive TBD_COMMIT | ssh xorinox 'mkdir -p ~/efs-f1 && tar -xm -C ~/efs-f1'
```

```bash
ssh xorinox 'cd ~/efs-f1 && make -j16 test-bins && export TMPDIR=$HOME/efs-tmp && make leak-check && make tsan-check && make asan-check && make test-all'
```

The 20-minute RSS run (the driver samples RSS once a second into its JSON;
the `ps` sampler is the independent series `scripts/rss_plateau.py` reads):

```bash
ssh xorinox 'cd ~/efs-f1 && export TMPDIR=$HOME/efs-tmp && mkdir -p out && (while true; do echo "$(date +%s) $(ps -eo pid=,rss=,args= | grep -E "efs-f1/(tests/faults/efsd|efs-fuse)" | grep -v grep | awk "{printf \"%s:%s \", \$1, \$2}")"; sleep 10; done > out/ps-samples.txt &) && EFS_D4_LOAD_S=1200 EFS_D4_ARTIFACTS=$PWD/out python3 tests/test_d4_progress.py && python3 scripts/rss_plateau.py out/ps-samples.txt --load-s 1200'
```

Expected: `VERDICT: PASS` from each gate and `VERDICT: PLATEAU`. Raw
outputs of this session: xorinox `~/efs-artifacts/f1-gates-20261025/`
(`SHA256SUMS` `TBD_SHA`): per-gate logs, every sanitizer report file, the
D4 samples and JSON, the first-run logs that showed the findings
(`*-run1*`, `asan-run2`). Cleanup: `edelete --force --delete ~/efs-f1 ~/efs-f1-tsan ~/efs-f1-asan`.

## Limitations

- `tests/test_rdma_memcheck.py` needs an RDMA device: NOTRUN here.
- TSan over the daemons covers the loopback TCP transport only.
- The leak gate checks parent processes (forked crash-simulation children
  are silent, K26); daemon leaks are covered by LeakSanitizer at clean
  exits in `make asan-check`, not by memcheck.
- Fixes 1 and 2 have the gate entry that found them as regression test;
  neither is reachable from a unit binary (`rv_*_watch_pass` are static in
  `recovery.c`; the cookie leak needs a process exit under LSan).
