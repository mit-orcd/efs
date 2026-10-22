# Five TCP package acceptance — NUC, Oct 7, 2026

## Purpose and linked item

Acceptance evidence for five grouped TCP work packages on NUC: W54/W38 (IOR/
hardscan + cold GC/restart), W56/W60 (root-rename reuse + mixed reads), W65
(writer-active-at-signal shutdown), W84 (namespace depth envelope), D26/W44
(inode-GC reap timing/fan-out investigation). Linked status ledger:
`docs/status/tcp-five-gates-20261007.md`. Run added by commit `ce0582a0f`
("docs(status): reconcile five TCP gate packages and retain NUC evidence").

## Verdict

Mixed, exactly as recorded in `outcomes.json` and the status ledger:

- W65: PASS (TCP scope) — six real concurrent fsynced-write leader stops (3
  direct, 3 buffered), all exit 0; acknowledged bytes verified after cold
  restart.
- W84 depth/guard envelope: PASS — depth143 accepted / depth144 refused,
  64-lane rmdir/replacement, coordinator-death fixture passes after LOOKUP fix
  (`namespace_coordinator_fault: 0`); RDMA deferred.
- W56: PASS (closes inside its package) — root-rename regression 64 repeats,
  mixed reads zero failures; repaired client `cb5e86de` verified on xorinox.
- Retained final full Linux `make test`: PASS (`retained_final_units: 0`).
- Retained failures kept visible: `namespace_coordinator_fault_original: 1` and
  `namespace_coordinator_fault_mapper_only: 1` — the two original failing runs
  (cold ENOENT after durable COMMIT; FUSE errno mapping defect) are preserved.
- D26/W44: NOT CLOSED — fan-out trial reverted; W54/W38, W60, W84 full-ancestry
  gates remain open (see Limitations).

## Source commit and build identity

Baseline commit `cd0cc4c5`. Per-fixture source and binary hashes are in each
fixture manifest (`gc-*/manifest.json`) and consolidated in `SHA256SUMS`;
`final-source-binary-provenance.txt` records sha256 of kept source files
(`raft_host.c`, `efs_fuse.c`, `gc_reclamation.py`), the kept/fault-final `efsd`
binaries and the retained `perf.data`. Repaired client verified on xorinox:
`efs-fuse` sha256 `da3101d9…44c2`, reporting `v0.1.0-pre-alpha-151-gcb5e86de`.

## Environment

- NUC host, private four-node loopback TCP fixtures; SATA `/data1/efs` and NVMe
  `/data2/efs` (the latter resolves through home). Shared cluster stores
  unchanged.
- Both buffered and direct I/O shutdown fixtures; VMs off.
- Client verification on xefsct1 (xorinox 3-node libvirt cluster), read-only.

## Method

Per README: `tcp-five-main.log` runs root rename/mixed reads with namespace and
GC/restart controls; `tcp-five-depth.log` runs depth143 acceptance, depth144
bounded refusal, 64-lane rmdir/replacement and cold restart;
`tcp-five-shutdown-{direct,buffered}-{1,2,3}` run writer-active-at-signal stops
with post-restart verification, physical GC, late-PUT fencing and zero final
quota. Perf (59,344 samples, zero loss) and strace (all four private daemons)
are separate runs against the owned main fixture, not untraced latency
benchmarks. The coordinator-death fixture uses a compile-only transaction
barrier (`EFS_FAULTS=1`, arms only for ≥64 participants, 15 s bound); normal
service builds exclude it. Tests kill at-66 participants, not the exact-144
maximum. Failed fixtures were cloned (`namespace-replay-*`) before repair to
preserve the original stores.

## Results

| Check | Result |
|---|---|
| fsync/fdatasync calls traced (4 daemons) | 28,393 successful; median 0.188 ms; max 19.46 ms; 147 calls >10 ms |
| Shutdown repeats | 6/6 exit 0 (3 direct + 3 buffered) |
| Depth envelope | depth143 accepted, depth144 refused in 8.004 s |
| Coordinator-death fixture | passes with 66 participants, cold atomic recovery |
| Inode-GC fan-out trial | 256 files reaped in 19.826 s vs 21.409 s serial; create/append still spike 47.074/59.535 ms — reverted |
| Settled reap timing (serial vs candidate) | fencing 11,617 vs 8,285 ms; lane sweeps 2,254 vs 2,402 ms; REAP_DONE 1,105 vs 1,414 ms |
| Final retained units | full Linux `make test` pass incl. both extracted LOOKUP regressions |

## Limitations and remaining gates

- W54/W38: nine physical hosts still owed; RDMA deferred (not passed).
- W60: historical full-tree coverage open (original `00*.dat` tree absent).
- W84: kills at 66 participants, not exact 144; finite ancestry product limit
  undecided.
- D26/W44: fan-out candidate archived under `source/` only, not activated;
  all-member success and durable fencing remain mandatory.
- Full `perf.data` remains at `/data1/efs/tcp-five-gc.data` on NUC (recorded
  sha256 in provenance file), not in the repo.

## Evidence index

| File | sha256 (16) |
|---|---|
| outcomes.json | 91f73c705ce9683b |
| README.md | ef5c63530eb2ea15 |
| sync-summary.json | 78bd9fe9579fc5e4 |
| final-source-binary-provenance.txt | 84123c5c91cf1456 |
| xorinox-build-verification.txt | 8b3c2fe46bfc08e7 |
| tcp-five-main.log | 84f3b27a78035acf |
| tcp-five-depth.log | 9e7ea604b6c54f40 |
| tcp-five-final-retained-units.log | a39fbea8c908a098 |
| tcp-five-reap-baseline.log | 3516affaeeb9730e |
| tcp-five-reap-candidate.log | 5c08f79aa7ca4fdc |
| tcp-five-namespace-retry.log | 6c387a9e710571e0 |
| SHA256SUMS | 98aff42084f928b9 |
