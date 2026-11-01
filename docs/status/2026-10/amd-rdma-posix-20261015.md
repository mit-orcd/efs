# AMD ordinary POSIX over RDMA/direct I/O — October 15, 2026

**Overall loaded qualification FAIL; ordinary serial/peer/cold/degraded and full native gates PASS.** Work is isolated
on `devel-amd-posix-20261015`, forked from local devel
`761c91c429708540b2e2f29c9c6b1709a9f00a87`. No seven-client workload is run.
This is AMD functional Soft-RoCE evidence under W106/W119, not ORCD hardware
qualification or closure of the original ORCD incident.

## Profile and changes

AMD `amd-395`, Ryzen AI MAX+395 / Radeon8060S,122108MiB RAM, Ubuntu
kernel7.0.0-38-generic and XFS backing storage; four same-host daemons on 127.0.0.1:17432–17435,
files storage, direct storage I/O, rxe0/GID1 strict segmented RDMA. Installed
stores have 17 GiB logical quota per node and were erased before initial
deployment with the user's authorization. Two installed mounts are
`/data1/efs/mnt` and `/data1/efs/mnt2`. Commands are disabled on clients AND
servers; expiry, experimental repair and grouped PUT
cohorts are off. Metadata balancing and the existing AMD GC-admission
experiment remain explicit. No application TCP comparison is performed.

Logical commits:

- `f6235bb73`: repair the extracted LOOKUP host fixture and check returned
  reader identity with/without server context; add explicit `single` mode to
  the four-node qualification driver, avoiding its default seven-client path.
- `85a9ad6b5`: regenerate stale architecture outputs. Full native suite PASS
  on this candidate; this is not a pass transferred to later production fixes.
- `cd3590c67`: service queued read probes at completed receive-batch handoffs,
  before another group's persistence; preserve KV hold and receive credits.
  Executed handoff regression passes and rejects the previous source.
- `6a9b18b2a`: standalone leadership probes carry no follower commit work.
  The leader retains its captured read index and own-apply barrier; normal
  replication heartbeats retain commit propagation and original cadence.
  Full in-memory Raft regression passes; prior-source negative fails.
- `8e0145e84`: prefix-zero contact may vote for an already-known leader while
  an ordinary append persists. Only stable existing term/boot/voters/config
  qualify; no pending log bytes are acknowledged and no follower read
  authority is granted. Hard-state/truncate/configuration and all protocol
  mutation remain fenced. Real stalled-persistence tests cover cohort/single/
  multi/conflict/hard-state paths, send/sync failures, wrong term/boot/leader/
  prefix/commit/route, independent-group progress and failed-sync fencing.
  ASan/UBSan and prior-source negative controls pass their selected scopes.
- `4591e5451`: enable existing duplicate-ACK window protection for ordinary
  files too. A reply below a newer outstanding window no longer retires that
  window and re-drives redundant disk work. Actual Raft tests check partial
  progress, old duplicates, lost-newer-ACK timer recovery, fresh read context
  and commit/apply; sanitizer and forced-old-retirement negative gates pass.
  Other optional batching policies retain their previous defaults.

- `c8d057391`: reuse only the already-certified fact that this leader has an
  own-term committed entry, instead of re-entering the shared disk-log mutex
  on every quorum getter, view publication and read completion. Certificate
  term/config/index bounds remain checked; new rounds still require new ACKs
  and their own applied floor. A real-core regression counts zero log GETs
  across 1,000 certified inspections/new admission and refuses a fresh read
  until its majority arrives. Prior source fails that counter assertion.

## Retained failures and diagnostics

On `85a9ad6b5`, installed serial POSIX PASS 216/216 in110.6s; one-mount
jobs16 FAIL 214/216 in68.5s: `dir_deep_nesting` and `names_crazy_dirs` time
out at the unchanged15s case deadline. Extras PASS1/SKIP4 (commands absent
and unsupported writable mmap), peer PASS 67/67, cold prepare and verify
PASS 27/27 each. Paused-follower read PASS: all32MiB, four full8MiB reads,
SHA256 `3cab1a5c8ff63b5d9d7375234c03a5fcae7beabdb5b600210586d07aa1deb586`.

A30s strace attached four daemons and both FUSE processes, while a separate
file-syscall trace followed test workers. This diagnostic perturbed execution:
209/216 passed. It is not acceptance. The deep worker spent14.19s in traced
file syscalls before termination, with progressively longer mkdir traversals.
Existing metadata counters put LOOKUP time mainly in fresh quorum waits.
Low-overhead counters showed read-only probes queued9–17ms on followers,
versus much smaller sender-queue/transport observations; those samples locate
avoidable inbox delay without claiming the entire case has one cause.

Intermediate quiet `cd3590c67` runs FAIL 214/216 and213/216. Quiet
`6a9b18b2a` FAIL 213/216. Quiet `8e0145e84` improves the selected run to
215/216, with unusual-name directories still timing out. These are different
runs on a retained namespace, not a controlled throughput benchmark; none
closes loaded acceptance. A later Python-only syscall diagnostic on8e also
records EBUSY in deep removal/create-unlink and timeouts. Its unusual-name
worker reached25 directories before termination; open/openat took8.67s,
mkdir2.92s and close2.00s across its traced calls, with no removal phase
reached. Treat these as diagnostics, not content corruption evidence.

A separate fresh4G/node fixture on8e also fails loaded POSIX207/216 after serial
PASS 216/216 in 185.4s. An initial direct invocation was NOTRUN (the tracked
script is not executable); the recorded run explicitly uses `bash`. Its later diagnostic stages overlap a newer candidate's build
and are not quiet qualification. Store/session history alone cannot explain
all failures. Raw failed evidence remains external; it is not replaced by
later passing repetitions.

The combined `4591e5451` passes full native testing but its fresh installed
quiet jobs16 round still FAILs214/216 (deep and unusual-name directories).
Its first failed round stops further stages; later candidate peer/cold results
must be rerun rather than borrowed from85a. This led to the certified-term
log-lock investigation above; original failures remain recorded.

## Reproduction

Use an exclusive AMD host as nonroot `efs`, with libibverbs, libfuse3, rxe0
and usable GID1. Build an exact committed source export, recording full SHA
in `SOURCE_COMMIT` and binary SHA256s:

```sh
make -j8 EFS_GIT_ID=c8d057391 EFS_GIT_DESCRIBE=c8d057391 \
  EFS_GIT_BRANCH=devel-amd-posix-20261015 efsd efs-fuse efs-mgmt test
export EFS_CLUSTER=amd EFS_TRANSPORT=rdma EFS_RDMA_DEV=rxe0
export EFS_RDMA_GID_INDEX=1 EFS_DIRECT_IO=1 EFS_MD_RAFT_N=4
export EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0
export EFS_EXPIRY=0 EFS_REPAIR_EXPERIMENT=0 EFS_PUT_COHORT=0
export EFS_METADATA_BALANCE=1 EFS_DATA_GC_ADMISSION=1 EFS_GC_BACKLOG_IDLE_MS=20
unset EFS_RPC_PROF EFS_READ_PROF EFS_BATCH_OBS EFS_DEEP_PATH_PROF
```

On the installed cluster, run core with `--jobs 16` and `--jobs 1`, each
with a unique tag/results path. The final mixed run preceded serial validation;
the initial85a baseline used serial first. Preserve this ordering in any
matched comparison. Do not override stock case
budgets (15s ordinary,75s decorated cases). Run
`tests/posix/posix_extras.py`, then
`tests/posix/posix_2client.py --local /data1/efs/mnt /data1/efs/mnt2`.
For durability use `tests/posix/posix_persist.py /data1/efs/mnt --phase prepare
--tag TAG`, cleanly stop all mounts/daemons with the AMD devops helper, start
the same binaries/stores, then run `--phase verify --tag TAG`. Keep the
persistence suite's existing30s deadline. Stop scheduling mutations if the
harness reports a live timed-out worker; retain its evidence and drain first.

For the installed profile, the exact suite commands (from the source root) are:

```sh
out=/data1/efs/amd-posix-reproduction-$(date +%Y%m%dT%H%M%S)
mkdir -p "$out"
for jobs in 16 1; do
  python3 -u tests/posix/posix_suite.py /data1/efs/mnt --suite core \
    --jobs "$jobs" --tag "amd-rdma-$jobs" --results "$out/core-$jobs.tsv"
done
python3 tests/posix/posix_extras.py /data1/efs/mnt --results "$out/extras.tsv"
python3 tests/posix/posix_2client.py --local /data1/efs/mnt /data1/efs/mnt2 \
  --parent amd-rdma-peer --results "$out/peer.tsv"
python3 tests/posix/posix_persist.py /data1/efs/mnt --phase prepare \
  --tag amd-rdma-cold --results "$out/persist-prepare.tsv"
EFS_CLUSTER=amd /data1/efs/devops/stop.sh
EFS_CLUSTER=amd /data1/efs/devops/start.sh
python3 tests/posix/posix_persist.py /data1/efs/mnt --phase verify \
  --tag amd-rdma-cold --results "$out/persist-verify.tsv"
```

Record every exit status and summary even after an ordinary completed failure;
do not run a later stage if workers remain alive or normal drain refuses.
Use a unique persistence tag when retaining multiple preparations. The shell
variables and flags above must be exported into both startup helpers.

Run the existing degraded oracle independently:

```sh
python3 tests/live/rdma_degraded_read.py /data1/efs/src /data1/efs/mnt OUT \
  --base-port 17432 --pid-root /data1/efs/logs
```

It restores the follower first in cleanup and drains its private reader mount;
its outer180s and original read/pair deadlines are unchanged. No seven-client
script is needed. For an alternative isolated4G/node reproduction:

```sh
EFS_GATE_LOAD_MODE=single bash tests/live/rdma_four_node_qualification.sh
```

The default mode of that driver is still seven clients: `single` must be set.
It owns only new private stores/mounts, performs clean teardown and removes
synthetic stores only after normal drain. Do not discard failed test evidence.

Focused model checks are `tests/test_host_batch_handoff.py`,
`tests/test_read_control_host.py`, `tests/test_queued_read_control.py`,
`tests/test_receive_persist.py`, `tests/test_raft` and
`tests/test_raft_ack_window.py` (a Raft policy regression). For the barrier sanitizer check set
`CC='cc -fsanitize=address,undefined -fno-omit-frame-pointer'` when invoking
`test_receive_persist.py`. The receive and handoff scripts support prior-source
negative overrides `EFS_RAFT_CORE_SOURCE` and `EFS_HANDOFF_SOURCE_REF`.

## Final exact-source results

Tested/deployed product commit: `c8d057391ccb81eccac7e4047cc565bfcd84acdd`,
build branch `devel-amd-posix-20261015`, build time2026-10-15T19:19:10Z.
All 945 tracked build/test inputs under src/include/tests/scripts/deps plus
Makefile match the frozen commit. The final checkpoint adds documentation and removes trailing whitespace from
a test assertion; it does not change the deployed executable source.

| Gate | Final result |
| --- | --- |
| Complete `make -j8 ... test` | PASS, exit0; all terminal recipes reached |
| Real stalled receive, AMD ASan/UBSan/leaks | PASS; cohort/single/multi/conflict/hard-state and failure matrix |
| Strict rxe0/GID1 transport smoke | PASS:68 RDMA application messages, `tcp_big=0`; no device SKIP |
| Installed core, jobs1 | PASS 216/216,111.5s |
| Installed core, jobs16 on one mount | **FAIL 215/216**,62.1s; only `names_crazy_dirs` exceeds15s |
| Further planned mixed rounds | NOTRUN after first failing round; no passing repetition substitutes for it |
| Isolated `names_crazy_dirs`, jobs1 | PASS 1/1,1.8s; does not close the mixed-load failure |
| Optional extras | PASS 1 / FAIL 0 / SKIP 4: unsupported writable mmap and disabled command checks |
| Two independent installed mounts | PASS 67/67,41.1s |
| Persistence prepare / all-process cold verify | PASS 27/27 in 22.3s /PASS 27/27 in 13.3s; normal stop, no discard substituted |
| Paused shared follower, fresh reader | PASS 32 MiB and exact SHA256 above; four full8MiB reads and EOF; follower restored |
| Runtime/executable identity and feature profile | PASS: four daemons/two clients, files/direct, strict RDMA uverbs, flags0, `.efs` absent |

SHA256 of installed binaries:

- `efsd`: `28b5e38ff4ee887048c84c2faab859c6c2e979c2e6e6efe4e9f1c58516bcfbad`
- `efs-fuse`: `4f27dfd8317cd16eef93e0db80d062259ed7842a2b6fc4ec8f6a2d63f54825d8`
- `efs-mgmt`: `29a788af74525bd46761d59f180572d950b21b2a2a99d883444aa3b04215cae3`

The degraded reader's four read syscalls total 2.592s, excluding path open,
setup, caller verification and cleanup. The whole degraded controller retains
its180s outer bound. 392 recorded candidate connection-counter lines have zero
post-setup TCP application bytes and zero violations; this count covers emitted
records, not a claim that every active connection has been retired. All six
current services have live uverbs descriptors and verified flags. Final four
daemons are running, unpaused, and the installed namespace is empty after
normal test cleanup. The final native/transport checks have finished; no load,
tracer, profiler or private fixture is left running.

## Evidence locations and remaining gates

Raw logs/traces/TSVs stay on AMD under `/data1/efs/amd-posix-20261015-*`,
with build/native logs `/data1/efs/amd-posix-*-build.log` and
`/data1/efs/amd-posix-*-native.log`. The isolated diagnostic uses
`/data1/efs/evidence/rdma-qualification-Atc0aV`; its mount root is recorded there.
No raw artifacts are committed. Final native logs are
`/data1/efs/evidence/amd-posix-term-native.log` (exit0), strict transport log
`/data1/efs/evidence/amd-posix-term-rdma-smoke.log` (exit0), and selected AMD barrier
sanitizer log `/data1/efs/evidence/amd-posix-term-receive-sanitizer.log` (exit0). Final mixed-load evidence is
`/data1/efs/evidence/amd-posix-20261015-term-results`; serial/peer/cold/degraded,
binary hashes and verified runtime are in
`/data1/efs/evidence/amd-posix-20261015-term-final-results`.

**Next required work:** instrument the unusual-name worker's retained CREATE,
first-touch session REGISTER/ESTABLISH, guarded OPEN, PUT/REPORT and CLOSE under
this exact one-mount mixed workload. Correlate caller PID/operation identity
and phase times before changing admission. The earlier trace stops during
creation/readback, so optimizing recursive removal alone cannot close it.
Distinguish fresh read waits from durable command settlement and receipt-window
backpressure. Preserve exact identities, fresh epoch/fence checks and original
15s/75s deadlines; rerun the whole mixed suite quietly after a demonstrated
critical-path fix. Do not close W119 from the isolated1.8s pass or widen the
budget. The present branch is not a fully green loaded candidate.

W120's extracted LOOKUP defect is repaired; enabled-command cleanup ownership
is NOTRUN here because commands are excluded. W120 remains open for that
independent gate. W119's original ORCD failures and W106 hardware/cross-host
obligations are not closed by AMD software-RDMA evidence. Seven-client load,
queries, expiry and grouped fragment durability are
explicitly NOTRUN/out of scope.
