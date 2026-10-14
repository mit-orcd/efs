# Default Raft queue guard: private Intel qualification

Updated **2026-10-14, America/New_York**. Linked gate: [W100](../backlog/work-items.md#w100).

**Aggregate: FAIL — directory-depth and shared-pwrite deadlines remain.** The admission and
failure regressions pass; normal seven-client repeats finish **1506/1512** and
**1507/1512**, with only `dir_deep_nesting` exceeding its unchanged 30s budget.
Bounding the queue is not a demonstrated correction for those deadlines.
The final cross-host peer suite also fails66/67: `peer_shared_pwrite` sideA
step1 exceeds its unchanged15s SSH step budget. Three quiet focused repeats subsequently pass under unchanged limits; the original failed gate remains retained.

## Source and behavior

Guard **`1ac603128`**, branch `devel`, is the exact Intel runtime/test source.
Baseline is deployed `61e5c939d`, documented at
[the preceding Intel checkpoint](intel-devel-merged-qualification-20261013.md).
No prerequisite merge was needed: the core already retries refused work and
sends fresh read probes while data admission is blocked.

The host now installs `send_ready` unconditionally and checks the pending
ordered-frame counter even when experimental flow is off/inactive. A second
queued ordered frame for the same peer/group returns AGAIN; the first frame
and its bytes remain owned and unchanged. Other peers/groups remain independent.
Dequeue releases admission; the already-dequeued transport pipeline is unchanged.
Snapshot chunks share ordered admission. Heartbeat/read control, votes and
replies remain eligible. Refusal grants no replication/commit credit; Raft
retries from its log. No timeout, retry, configured transport-window, pacing, protocol,
cache or durability changes.

A parallel human-authorized AMD integration advanced `devel` to `a40428d9d`,
which includes this guard. Intel qualification stays pinned to `1ac603128`
for attribution; the later AMD/query source changes are not claimed tested by
these Intel results. The merge/status history remains separate in
[the AMD merge checkpoint](devel-rdma-merge-qualification-20261014.md).

## Environment and results

Intel1 `10.0.0.207`, Intel2 `10.0.0.209`, Core Ultra 7 256V, eight CPUs/~15GiB
per host, Rocky Linux 10.2 / 6.12.0-211.64.1.el10_2.x86_64. Four cross-host
private daemons, two per host, ports27432/27433, files engine/direct I/O,
16GiB quota/node. Existing stores retained without mkfs. Opposite-host seeds;
main mount `/data1/efs/w100-private/runtime-merged-aa508c820/mnt` on each host.

`EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0
EFS_EXPIRY=0 EFS_MD_RAFT_N=4`; files engine disables containers. Experimental
Raft flow remains off, confirmed from the actual host structures during sampling.
Production ports17432/17433 and `/data1/efs/mnt` are not replaced.

| Gate | Result |
| --- | --- |
| Full native `make -j8 test`, both hosts, exact1ac603128 | PASS, exit0 each |
| Actual host enqueue/prebuild regression, normal and targeted ASan/UBSan/leaks | PASS both hosts; not full-library sanitizer coverage |
| Old host source runtime negative | Expected assertion failure admitting a second ordered frame; compilation succeeds |
| Seven clients, 4Intel1/3Intel2, 16jobs each, round1 | FAIL1506/1512: depth60timeouts on all Intel1 clients, Intel2 clients1/2 |
| Same normal-profile seven-client repeat, round2 | FAIL1507/1512: depth60timeouts on Intel1 clients1/2, all Intel2 clients |
| Read-only queue sample during round1 and paused-node workload | Maximum pending1/group/peer, queued entries2/peer; flow off all four daemons |
| Baseline daemon3 pause10s, 64fsynced1KiB files/host, eight workers/host | PASS128 exact files from both mounts; maximum case4.06s |
| Guard daemon3 pause10s, same file/worker counts | PASS128 exact files from both mounts; maximum case6.039s/4.051s on Intel1/2 |
| Cold verification after all four daemons and both clients restart | PASS both128-file fixtures, exact bytes from both hosts |
| Existing cold persistence suite | PASS27/27 per host |
| Final cross-host peer | FAIL66/67; shared-pwrite sideA step1 timeout15s |
| Focused unchanged shared-pwrite repeats | PASS3/3;12.9s,13.3s,12.1s; original full-suite failure retained |
| Full isolated GC/fencing/crash/recovery, 4GiB/node | PASS complete exit0 both hosts; core216/216 and peer67/67 each |
| Live two-fsync EOF omission/refusal/recovery, both directions | PASS512B and8MiB+1, two EIO/peerEOF0 then exact recovery after clearing omission |

All failed concurrent cases retain their original limits/verdicts. Every other
case passes. Raw results are `concurrent-*-queue1ac-round{1,2}-*.tsv`.
Previous61e5 loaded1510/1512 also had2000 extra O_EXCL cases and a different
retained-store state: these are not matched performance A/B runs. The queue
sample supports the bound in that interval, not a causal latency improvement.

The native tests execute actual production functions with transport/encoding
adapters. They cover flow-off/inactive/active admission, independent groups and
peers, snapshot FIFO, original-frame retention, control escape, failed sender
creation rollback and stopped-host refusal. Existing Linux async sender tests
retain lost-reply, malformed feedback, reconnect, mixed transport and stop gates;
the Raft core suite verifies refused data earns no commit credit, fresh reads
remain available and replication retries after admission reopens. An initial
sanitizer invocation used the wrong CC argument convention for `test_host_flow`;
corrected invocation uses `EFS_TEST_CFLAGS`, with no assertion/code changes.

## Reproduction

Prerequisites: existing owned private four-node fixture, Intel SSH aliases,
Python3 and a C toolchain. Pause injection requires Linux pidfds and the same
UID as the owned daemon. Sampling additionally needs gdb and same-UID read
access to `/proc/PID/mem`; gdb reads binary types and **never attaches**.

Use Mac controllers from this qualification checkpoint; remote daemon/native
sources stay frozen at1ac603128. Build an exact `1ac603128` source archive on both hosts under
`/data1/efs/w100-private/devel-1ac603128`; preserve historical tracked `results/`
needed by native documentation gates. Supply explicit identity to every make:

```sh
make -j8 EFS_GIT_ID=1ac603128 EFS_GIT_DESCRIBE=1ac603128 EFS_GIT_BRANCH=devel all
EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_EXPIRY=0 EFS_RAFT_FLOW=0 make -j8 EFS_GIT_ID=1ac603128 EFS_GIT_DESCRIBE=1ac603128 EFS_GIT_BRANCH=devel test
CC='cc -fsanitize=address,undefined -fno-omit-frame-pointer' ASAN_OPTIONS=detect_leaks=1 python3 tests/test_host_pending_entries.py
CC=cc EFS_TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' ASAN_OPTIONS=detect_leaks=1 python3 tests/test_host_flow.py
```

Old-source negative: `git show 7c5a59f51:src/server/raft_host.c > /tmp/old-host.c`,
then `EFS_RAFT_HOST_SOURCE=/tmp/old-host.c python3 tests/test_host_pending_entries.py`.
It must fail the runtime redundant-frame refusal assertion.

From the Mac, stop the existing private source with `intel_w100_private.py stop`
then resume the same run from the new source (never use `start` on retained stores):

```sh
python3 scripts/intel_w100_private.py resume merged-aa508c820 devel-1ac603128 --quota 16G
python3 scripts/intel_package3_tests.py concurrent --run-id queue1ac-round1 --output /tmp/intel-queue-results --source-root /data1/efs/w100-private/devel-1ac603128 --mount /data1/efs/w100-private/runtime-merged-aa508c820/mnt --seed-port 27432 --remote-output-root /data1/efs/w100-private/results --extra-mount-root /data1/efs/w100-private/extra-mounts
python3 scripts/intel_queue_sample.py --source /data1/efs/w100-private/devel-1ac603128 --seconds 30 --output /tmp/queue-samples.json
python3 tests/live/private_intel_queue_pause.py --source /data1/efs/w100-private/devel-1ac603128
```

Run sampling alongside a workload. Repeat the concurrent driver with a unique
`queue1ac-round2` run-id; no observer or additional test workload in either round.
The sampler collects300 samples100ms apart per daemon, deriving offsets from
its actual binary and checking process identity/UID and flow-disabled state.
It does not attach, stop, reconfigure or change logging on daemons.

The pause controller verifies node3 PID/executable/storage ownership through a
pidfd. A remote timer resumes it after10s in a finally block; the controller
waits for that timer even on graceful cancellation. It injects no app retries;
each create/write/fsync/close must finish within30s. All files remain in the
printed unique `queue-pause-*` fixture. After a clean private stop/resume,
repeat with `--verify FIXTURE` to check exact bytes from both hosts without
signalling. Remove only those owned fixtures after the qualification is accepted.
Concurrent driver-created clients drain/stop in its finally block.

On each Intel host, from the frozen source, isolated failure qualification is:

```sh
EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_EXPIRY=0 EFS_RAFT_FLOW=0 timeout 1800 python3 tests/live/gc_reclamation.py --mode direct --transport tcp --disable-efs-commands --fixture-quota 4G --posix --posix-jobs 16 --continue-posix-failure --guarded-leases --opid --integrity --namespace --allocation --tiny-churn --full-node
```

These are separate four-daemon loopback fixtures per host, not cross-host
transport proof. The gate covers exact REPORT/session/opid fencing, guarded
leases, full-node refusal/reroute, integrity/namespace/allocation/tiny churn,
disk-error retention, offline REAP retention, crash/cold replay and final
physical/quota inventory. Failed fixtures remain; successful owned mounts and
processes are cleaned up. This is process-crash, not physical power-cut evidence.

## Evidence and remaining gates

Raw evidence is outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-queue-guard-20261014`.
Remote logs are under `/data1/efs/w100-private` (`1ac-native.log`, `1ac-gc.log`).
Both staged sources match all1278 non-results committed files with zero mismatch.
Actual version RPC confirms build times2026-10-14T05:04:34Z on Intel1 and
05:04:33Z on Intel2 (Oct14 01:04 EDT), branch devel. Final deployment manifests
include server/client PIDs, runtime arguments, environment and binary SHA256:

- Intel1 efsd: `b8fdbb441cb28410a848400510f23eb2f7785737773ede5ca2a0403808fd9fa6`.
- Intel2 efsd: `d2e1008106a9dcea2249f2bafbdd6aeb71a2a9b564efe6cd79e6c8532dac1531`.

W100 remains OPEN. The next directory correction requires a demonstrated
unnecessary wait with namespace/read-authority proofs intact, followed by
repeatable loaded acceptance. Queue bounding alone does not justify closure,
timeout increases, stale metadata or a performance improvement claim.


Full failure suites complete with exit0 on both hosts. Their four-node
loopback fixtures include core216/216 and peer67/67 on each host, then mixed
append/GC, exact800 cold records, invalid WAL tail repair and subsequent crash
recovery, final zero quota/inventory and late-PUT fencing. The parallel main
fixture cold verification is an exact-bytes check, not a quiet latency baseline.
A one-frame queued bound can reduce available batching; no throughput or
latency benefit is established by passing safety/recovery tests.


Two-fsync qualification uses separate EFS_FAULTS clients built from1ac603128
against normal guard servers. `tests/live/report_eof_contract.py MOUNT FAULT
--expect refused --peer OTHER_HOST` runs sequentially in each direction.
512B and8MiB+1 cases refuse two fsyncs while omission is enabled and peers
observeEOF0, then recover exact EOF and checked bytes after clearing omission.
The sparse case checks anchors/EOF, not every hole byte. Both temporary fault
mounts drain/stop, fault files resetOFF. Logs: `intel{1,2}_efs-1ac-two-fsync.log`.


Final cross-host peer raw files `1ac-cross-peer.tsv`/`.log` retain the failure:
`peer_shared_pwrite`, `ab-a step1`, rc124, SSH timeout15s. This stock case
performs500 disjoint4KiB pwrite+fsync operations per host against a64MiB file.
No count/deadline changes were used. The peer round followed fault-client
qualification; three quiet focused repeats pass; no matched baseline comparison was run, so no causal
regression/improvement claim is made. This is an additional acceptance blocker.


Focused unchanged `peer_shared_pwrite` repeats PASS3/3, with suite durations
12.9s,13.3s,12.1s. The original full-peer timeout remains FAIL. The cause is not established and
no queue-guard regression/improvement claim follows from these unmatched runs.
Reproduce from Intel1 using the same `posix_2client.py --remote` invocation,
adding `--filter peer_shared_pwrite`, original wrapper15s and unique prepared
parents `queue1ac-pwrite-{1,2,3}`. Raw logs/TSVs `1ac-pwrite-{1,2,3}` are in the
artifact directory. No assertion, loop count, timeout or application retry changed.

Final main deployment remains normal1ac603128 with the required disabled
features. All extra concurrent/fault mounts and standalone GC daemons have
been drained/stopped; baseline/guard pause files remain for inspection. Production
PID/start-time/ports/mount evidence is retained in `intel{1,2}_efs-final-processes.txt`.
The queue controller and sampler are reproducible source assets, not raw captures.
