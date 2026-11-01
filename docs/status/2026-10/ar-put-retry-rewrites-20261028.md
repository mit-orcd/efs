# AR — PUT retries and identical-object rewrites: measured, no change (Oct 28–29, 2026)

Branch `agent/devel-taskar`, head `9b5de147e` (on devel `9c4bfd171`;
measurement assets only, no product change). Gate: `make test-all` on intel1
= **TOTAL 546 FAIL 1 NOTRUN 12 (696s)**; the tree is devel plus measurement
scripts, so the gate is itself the base comparison. The one FAIL is the
pre-existing devel b6 slow-mode defect (AK; identical F0 fsync-EIO signature,
write path, unrelated to AR). The 12 NOTRUN are the usual host limits (no IB
device, no valgrind headers, no cluster scripts, sanitizer probe). Defects
found by this task: none. Named AR gates PASS: store overwrite, PUT ticket,
PUT upload server, PUT ticket RPC.

## Verdict

**Measured, no change (record and stop).** The threshold is retries under
1 % of PUTs in both loads. Measured EEXIST rewrites (`ar_over`, a temporary
daemon counter at `server_write_fragment_to_path`'s EEXIST→`no_create`
branch, src/server/store.c:3112) were **0 in every load**: the D4 small-file
load, the `append_failover.py` baseline and the same gate with
`--lose-reserve-every 97 --lose-reserve-max 4`. No completion-proof or
verified-reuse design is built.

## What was measured

On EEXIST the exclusive-create PUT is rewritten whole (padded payload + I25
tail) without comparing the existing object — so a client retry after a lost
PUT reply writes the same immutable bytes twice. The question is how often
that happens in practice.

- `scripts/ar_d4_measure.sh`: private 3-daemon loopback cluster on intel1,
  `scripts/d4_progress.py` (8 threads, 120 s, create/unlink/rewrite small
  files + foreground 1 MiB write/fsync). **D4 PASS**, `ar_over=0` on all
  three daemons.
- `tests/live/append_failover.py --records 2000 --writers 4` (production
  build): verdict PASS, `ar_over=0` on all three daemons.
- The same with `--peer-writers 1 --lose-reserve-every 97
  --lose-reserve-max 4` (EFS_FAULTS=1 build): verdict PASS, 4 injected lost
  reserve replies, `ar_over=0` on all three daemons. PUT ops did rise
  (4,428 → 7,188 per daemon) — the retry ladder writes **new** objects
  (abandoned reservation identities retire as zero holes); it never re-PUTs
  an existing object.

| load | logical bytes | put bytes/daemon | wdisk bytes/daemon | EEXIST rewrites |
| --- | --- | --- | --- | --- |
| append baseline (2,000 × 4 KiB) | 8,192,000 | 290,193,408 | 290,193,408 | 0 (0 %) |
| append lost-reserve (2,000 × 4 KiB) | 8,192,000 | 471,072,768 | 471,072,768 | 0 (0 %) |
| D4 small-file load (120 s × 8 threads) | mixed | 1,128,857,600 | 1,128,857,600 | 0 (0 %) |

(put == wdisk in every load: every submitted fragment PUT reaches the disk;
the large put/logical ratio is the by-design fragment padding and metadata
tables, not retries.)

Write latency (append gate, per 4 KiB write, all writers, from the raw
`writer-N-latencies.json`): baseline p50 0.337 s / p95 0.676 s / p99 0.773 s;
with lost reserve replies p50 0.318 s / p95 1.011 s / p99 2.036 s — the
retry tail is the reserve-abandon ladder, not fragment rewrites.

## Counter validation (the zero is not dead code)

`scripts/ar_collision_check.sh` (fault build,
`EFS_FAULT_PUT_REPLY_COLLISION=1` on the mount) drops the first PUT reply of
each fragment of one chunk: the client log shows 3 `put-reply fault` lines
and each daemon then reports **ar_over=1, ar_bytes=65536** — exactly one
64 KiB fragment rewritten per node. The retry-rewrite mechanism is real and
the counter detects it; it simply never fires in the measured loads.

## Gates

Named AR gates on the clean (uninstrumented) build at `9c4bfd171` on intel1,
all PASS: `tests/test_store_overwrite`, `tests/test_put_ticket`,
`tests/test_put_upload_server`, `tests/test_put_ticket_rpc.py`. The
retry/conflict/quota/torn/interrupt/restart/barrier implementation gates are
not applicable: no product change was made, so there is nothing new to pin.
Full gate: `make test-all` on intel1: **TOTAL 546 FAIL 1 NOTRUN 12 (696s),
OVERBUDGET 8** — the FAIL is the pre-existing b6 slow-mode defect above.
Raw: intel1 `~/efs-artifacts/taskAR-20261029/test-all-summary.txt`.

## Reproduction

```
patch -p1 < scripts/ar-instrument.patch   # TEMP-AR daemon counters (never committed)
make -j8 efsd efs-fuse efs-mgmt
bash scripts/ar_d4_measure.sh . /tmp/ar-d4 120
# append baseline / lost-reserve: tests/live/append_failover.py with --rel .
# counter validation (fault build):
make -j8 EFS_FAULTS=1 efsd efs-fuse efs-mgmt
bash scripts/ar_collision_check.sh . /tmp/ar-collision
```

Restore `src/server/store.c` and `src/server/iostats.c` afterwards. Raw
artifacts: intel1 `~/efs-artifacts/taskAR-20261029/` with `SHA256SUMS`.
Environment: intel1 VM, loopback TCP, 8 CPUs, co-tenant taskAO load ~4–5;
the counters are load-independent by construction. The ORCD stage is not
informative here: the EEXIST counter fires only on a lost PUT reply, which
loopback and healthy real transports alike do not produce in these loads;
the collision check is the deterministic form of that event.
