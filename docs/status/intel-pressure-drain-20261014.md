# Intel indexed pressure and background draining — 2026-10-14 EDT

**Deployed; focused native/sanitizer and four-client smoke PASS.**
Final paired throughput evidence is below; full POSIX/crash acceptance NOTRUN.
Follow-up to [live pressure profiling](archive/intel-post-sync-profile-20261014.md).
Scope: devel, Intel TCP/direct-I/O, ordinary files; .efs client/server and
expiry disabled. Preserve storage and independent external clients.

## Change

Two fixed 8 KiB bitmaps index cache slots with pressure candidates and allocated
metadata chains. Each word belongs to a cache shard and uses its existing lock.
Dirty transitions, successful PUT-record updates and chain insertion set bits.
Selection revalidates the original ownership predicate; stale bits clear only
when no member qualifies, regardless of the selection cursor. Chains retain
unpublished, stalled and writer-owned metadata. No entry pointer escapes a lock.
The trim path takes 64 shard locks instead of 65,536 per-slot lock pairs.
This is a bounded slot index, not a dynamically allocated per-inode registry.

One joinable background worker consumes a coalesced wakeup, at most 16 inode
selections per pass, with the existing 30-second per-drain deadline. Each
background operation uses the append lock and existing snapshot/PUT/REPORT
ownership path under drain credit. It flushes at most `EFS_WRITE_PIPELINE` chunks
(currently 32), then publishes completed records without waiting for future
writeback jobs or all future PUT windows. It never consumes foreground writeback
errors. Explicit fsync still drains queued writes, every dirty chunk and all
prior PUT windows.
Successful foreground reservations remain reserved and can proceed at the early
high-water threshold; actual budget refusal retains bounded foreground draining
and errors. Worker creation failure also falls back to the foreground path.
Hard body/metadata limits, drain reserve and early threshold are unchanged.
Background failure does not acknowledge success or discard ownership. Explicit
fsync/close ordering is unchanged. Shutdown joins before writeback/client teardown.

## Qualification

Production-extraction tests:

```sh
python3 tests/test_pressure_index.py
python3 tests/test_pressure_background.py
python3 tests/test_pressure_admission.py
python3 tests/test_pressure_cohort.py
python3 tests/test_wb_drain_budget.py
python3 tests/test_append_flush_budget.py
EXTRA_CFLAGS='-fsanitize=address,undefined -g' python3 tests/test_pressure_index.py
EXTRA_CFLAGS='-fsanitize=address,undefined -g' python3 tests/test_pressure_background.py
EXTRA_CFLAGS='-fsanitize=address,undefined -g' python3 tests/test_pressure_admission.py
EXTRA_CFLAGS='-fsanitize=address,undefined -g' python3 tests/test_pressure_cohort.py
python3 tests/test_client_memory.py --sanitize
FOLD_TEST_CFLAGS='-fsanitize=address,undefined -g' python3 tests/test_fold_observation.py
python3 tests/test_load_merge_race.py
python3 tests/test_write_budget.py
python3 tests/test_report_retry_eof.py
python3 tests/test_report_pressure.py
python3 tests/test_report_zero_eof.py
```

These pass locally, including ASan/UBSan for index, worker, admission, cohort,
cache-memory and fold-observation harnesses. Linux-native builds and the four
pressure harnesses pass on Intel1, Intel2, AMD and xorinox. Intel1 also passes
native `tests/test_bufpool`, Linux sanitizer index/cohort/worker checks and the
existing WB/append deadline/error and EOF/REPORT regressions. Two older extraction
harnesses needed adapter updates for the new bitmap fields; their existing
ownership assertions remain and pass. Index tests compare every cursor with the former full-scan
oracle, preserve failed/unpublished ownership, rearm late PUT completion and
retire only idle chain nodes. Worker tests cover coalescing, serialized drain,
failure/retry and joined shutdown. Admission tests cover reservation preservation,
worker-unavailable fallback, quota/BUSY/EIO propagation and expired-call refusal.
The worker harness controls drain completion; it does not simulate a real server
crash or prove end-to-end cancellation semantics. Existing two-fsync EOF,
REPORT-pressure and synchronous-write error tests remain relevant.

Raw work and test evidence (outside Git):
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-pressure-drain-20261014/`.
Isolated native build: `/data1/efs/evidence/pressure-work-20261014` on Intel1.
Full POSIX/crash acceptance is not part of the requested smoke rollout.

## Final deployment and smoke

Production source `2e935cea075dfec45d25385bc8626c49fee67ba3`, build ID
`2e935cea075d`, version `devel-2e935cea0`, build timestamp
`2026-10-15T03:42:00Z`. Source archive contains Makefile/include/src/deps/scripts/tests;
SHA-256 `66ee2c3dc70a4eb344a42b3350a1f3a0f50646540e4d6108bc53e479eb783a62`.
The final documentation/harness-only commit does not change production binaries.

Native build command on each destination:

```sh
make -j8 EFS_GIT_ID=2e935cea075d EFS_GIT_DESCRIBE=devel-2e935cea0 EFS_GIT_BRANCH=devel EFS_BUILD_TIME=2026-10-15T03:42:00Z efsd efs-fuse efs-mgmt
```

Intel1 `.207` runs n1/n2, Intel2 `.209` runs n3/n4, ports 17432/17433,
400 GiB quota each. Existing storage paths and contents remain. Main releases
are `/data1/efs/releases/2e935cea075d`; AMD/xorinox clients are independent under
`/data1/efs/intel-client/releases/2e935cea075d`. All four daemons and all four
clients have verified executable identity and `EFS_TRANSPORT=tcp`,
`EFS_COMMANDS=0`, `EFS_COMMANDS_SERVER=0`, `EFS_EXPIRY=0`.
Daemons use `--direct-io`. Intel mounts are
`/data1/efs/mnt`, external mounts `/data1/efs/intel-mnt`.
Private Intel PIDs, executable hashes and profile match the pre-rollout inventory.

All mounts stopped through their drain-aware control scripts before daemons
stopped. Main Intel startup used explicit `launch`, `wait`, then `mounts` phases;
no format or storage reset. Catalog recovery took roughly two minutes.

All four clients created/read the four-role `intel_getattr_mount_smoke.py`
fixture and passed fsync, shrink/regrow, EOF, content and mtime checks. Each
client also matched all 64 preserved pre-rollout file samples by inode, size
and SHA-256. The new concurrent pressure smoke passed two 64 MiB writers,
full-content verification, two fsyncs, shrink by seven bytes, regrow by twenty,
zero-tail verification and exact EOF. Smoke fixtures were removed only on pass.

```sh
# On all four clients, create roles 0..3, then read on all, clean once:
python3 tests/measure/intel_getattr_mount_smoke.py MOUNT create getattr-lane-smoke-pressure-2e93 ROLE
python3 tests/measure/intel_getattr_mount_smoke.py MOUNT read getattr-lane-smoke-pressure-2e93 ROLE
python3 tests/measure/intel_getattr_mount_smoke.py MOUNT clean getattr-lane-smoke-pressure-2e93 0
# On AMD:
python3 tests/measure/intel_pressure_smoke.py /data1/efs/intel-mnt
```

These scripts require a live FUSE mount and disabled .efs. They create unique
owned files, preserve failed fixtures, and remove only their own passing files.
No new root packages or firewall rules were required.

## Durable AMD dd result: no established sustained gain

Baseline production code was `2256f1a34fc1006e3ecb9ab356a46a0341f0b605`.
Both versions use the same pre-generated 512 MiB random input, SHA-256
`ed5023f44ee84a6826e4a2e5930359070fe292a6007d75937f5b3a256c5b59c8`.
Each measured process writes a new file with `bs=1M count=512 conv=fsync,excl`,
then full SHA-256 and size verification run outside the timed interval.
The script removes only the newly created output after it verifies successfully.
All six final-release trials passed. No profiler ran during these trials.

| Phase | Individual MiB/s | Median MiB/s |
| --- | --- | --- |
| Baseline repeated set | 79.58, 72.62, 77.08 | 77.08 |
| Final release, first set | 82.30, 75.06, 80.09 | 80.09 |
| Final release, warm repeat | 78.99, 73.80, 75.57 | 75.57 |
| Final release, all six | 73.80–82.30 range | 77.28 |

The initial +3.9% median observation did not reproduce in the warm set (-2.0%).
The pooled median differs by only +0.26%, with overlapping ranges. **A sustained
throughput improvement is NOT ESTABLISHED.** The implementation removes full
pressure walks and permits background overlap, but we do not claim a measured
CPU reduction or a large dd gain from this rollout. Logs still show actual
hard-budget refusal near 252 MiB of live bodies and final fsync flush times
around 0.9–1.6 seconds in the warm set. Retained fragment durability and
publication work remain significant; no limits, retry windows or sync guarantees
were weakened to improve the number.

Limits: three baseline and six final samples, changing output inode/placement,
cache/catalog state and complete daemon restarts. Group0 stayed on node3
(Raft leader ID2) across both rollouts, but this is not a randomized,
leadership/cache-controlled A/B. An initial baseline set (44.61, 47.07,
46.21 MiB/s) overlapped the isolated build and was discarded; the repeated
baseline above had no full build/profiler, though brief native harness checks
ran early in that observation window. The first implementation was also
measured cold and warm; its failed improvement is retained below rather than
selected away.

```sh
# AMD, before rollout; after rollout change baseline to candidate.
python3 tests/measure/intel_dd_sync_batch.py /data1/efs/intel-mnt /data1/efs/intel-client/pressure-drain-measure baseline --mib 512 --repeats 3
python3 tests/measure/intel_dd_sync_batch.py /data1/efs/intel-mnt /data1/efs/intel-client/pressure-drain-measure candidate --mib 512 --repeats 3
```

The human dd had already finished before this task. The xorinox four-worker
MD5 crawl was gracefully interrupted for the comparison and restarted afterward
as PID 727786, with old output preserved and new output at
`/data1/efs/intel-client/logs/md5-pressure-drain-2e93-20261014.jsonl`.
Initial resumed evidence: 2,571 hashes, zero errors. This restarts traversal;
it does not claim to resume the exact prior position. Final management status
reports all four nodes up with identical `2e935cea075d` builds.

Local raw files include `amd-baseline-quiet.jsonl`, `amd-cohort-candidate.jsonl`,
`amd-cohort-warm.jsonl`, superseded candidate measurements, four-host build and
inventory reports, controls, mounted smoke results, native/sanitizer logs and
`final-health.txt`. The source archive is `release-2e935cea075d.tar`.
No raw captures or generated payloads are committed.

## Rejected first candidate

Initial implementation `843c1e2ae` passed mounted data/EOF checks but used the
foreground whole-inode drain in its worker. Three initial AMD trials were
61.16, 58.35 and 66.18 MiB/s; three warm repeats were 66.97, 69.95 and 69.03 MiB/s
(the raw JSON is authoritative for exact values). Buffers repeatedly reached
real hard-budget refusal. This did not improve the 77.08 MiB/s baseline median.
Source review also found that the foreground writeback wait consumes its sticky
error, inappropriate for background use. The follow-up replaces that path with
a fixed dirty cohort; the worker does not call the writeback wait at all.
Native `test_pressure_cohort.py` verifies the limit, full explicit pass, failure
handling and restored deadlines. The original foreground error and wait behavior
is unchanged and covered by the WB/append drain tests.
