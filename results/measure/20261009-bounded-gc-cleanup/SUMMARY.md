# SUMMARY — 20261009-bounded-gc-cleanup

## Purpose and linked item

Acceptance run for bounded GC/reap cleanup at code commit `978caaf7`: an
RPC-only SWEEP_NEXT request, REAP_BATCH entries carrying at most eight exact
reservation/receipt keys, and a fairness fix so partial progress on one lane no
longer starves later lanes. Part of the 2026-10-09 dependency-linked
correctness package (`docs/status/dependency-package-20261009.md`, "Implemented
cleanup portion" and "Installed deployment" sections). Added to raw-evidence by
commit `3dec69958`.

## Verdict

PASS for the bounded-cleanup acceptance and the installed AMD deployment of
`978caaf7`. Linux full `make test` passes; strict production decoder/verdict
harness, atomic batch fault, malformed cross-inode key refusal, exact replay
and durable LSM WAL reopen pass; final private GC gate passes POSIX 212/212,
peer 64/64, 800 cold append records, empty physical inventory/quota zero after
restart, and all-member late-PUT fence; installed deployment passes POSIX
212/212, peer 64/64, persistence 26/26.

One recorded intermediate FAILURE: the combined candidate (predecessor of the
fairness fix) failed the 180-second queue-drain limit (`gc-direct-w_n7wv_v`
failed-status in `raw-evidence.tar.gz`); fixed by scheduling other lanes after
bounded partial progress, then re-accepted.

## Source commit and build identity

- Code commit `978caaf7`, branch `devel-w66`; final candidate build label
  `w66-bounded-3` (committed source, before the identity-only release build).
- `installed-health.txt`: `efsd 978caaf7 (branch devel-w66, built
  2026-10-09T03:17:20Z on amd-395)`.
- Installed backup of previous baseline `ab050595` retained at
  `/data1/efs/evidence/w66-bounded-backup-20261009` (on-host, not in repo).

## Environment

- AMD cluster (host amd-395), 4 efsd daemons, 127.0.0.1:17432–17435, quota
  37.5G each, TCP transport, direct I/O, both command masks disabled
  (`EFS_COMMANDS=0`, `EFS_COMMANDS_SERVER=0`), all processes as user `efs`.
- Private direct/TCP four-daemon GC fixtures under
  `/data1/efs/evidence/w66-gc-fixtures/` (fixture ids `gc-direct-dlqb3a0v`,
  `gc-direct-_iqvbemo`, failed `gc-direct-w_n7wv_v`), borrowing installed
  mounts `/data1/efs/mnt` and `/mnt2` under the amd-live reservation lock.
- Note: local macOS full build cannot compile the existing fopencookie-based
  log_ts; Linux is the verification environment.

## Method

Scripts retained in the run dir: `efs-w66-reap-live.sh` (reservation, stop
installed clients, run `tests/live/gc_reclamation.py --mode direct --transport
tcp --posix --append-audit` on port 27640, restore mounts on exit) and
`efs-w66-deploy-bounded.sh` (backup, deploy `978caaf7`, rollback on
non-acceptance, reservation bookkeeping). Gates: strict decoder/verdict
fixture, failed atomic write/malformed keys, exact batch retry, LSM WAL
crash/reopen, private GC/restart, core POSIX, peer, 800 cold append records,
physical inventory/quota, late-PUT fencing. Installed smoke gates in
`w66-active-20261009/` (baseline `ab050595`) and `w66-bounded-20261009/`
(`978caaf7`). Nested raw archive `raw-evidence.tar.gz` preserves the initial
bounded-reap pass, the failed slow-drain candidate, and the final
fairness-fix fixture logs.

## Results

| Gate | Result |
|---|---|
| Linux full `make test` (978caaf7) | pass (`efs-w66-bounded-final-test.log` in archive) |
| Decoder/verdict, batch fault, malformed keys, replay, WAL reopen | pass |
| Private GC gate, final fairness-fix build | POSIX 212/212, peer 64/64, 800 cold appends, inventory empty, quota zero, PUT fenced |
| Combined predecessor candidate | FAIL — 180 s queue-drain limit (lane starvation), retained in archive |
| Installed baseline ab050595 smoke | POSIX 212/212 (17.9 s), peer 64/64, persistence 26/26 |
| Installed 978caaf7 smoke | POSIX 212/212 (18.1 s), peer 64/64, persistence 26/26 |

## Limitations and remaining gates

- Historical REAP_DONE/LANE_SWEEP semantics kept for replay;
  mixed-version/downgrade is not certified.
- Does not establish a universal memory/work bound and does not finish
  production publication. No D25 or pressure-policy activation; no hardware
  power-loss or RDMA claim.
- Next per README/status page: audit admission expansion charges, then mount
  sessions/PUT tickets and production typed publication ownership; W66
  generalized scaling remains open.

## Evidence index

| File | sha256 (16) |
|---|---|
| README.md | 7d832ef6b5ec2944 |
| installed-health.txt | edd8e2d3e72fb1fd |
| efs-w66-reap-live.sh | 3a654fd7d5046bb9 |
| efs-w66-deploy-bounded.sh | 1355d2b463d15708 |
| w66-active-20261009/verdicts.tsv | 49fb39bebe3e02d7 |
| w66-bounded-20261009/verdicts.tsv | 49fb39bebe3e02d7 |
| w66-active-20261009/smoke.log | c304c6e56bed4f69 |
| w66-bounded-20261009/smoke.log | 5322606b49d64ff4 |
| raw-evidence.tar.gz (nested fixtures/build/test logs) | 36ed83b414ce6f66 |
