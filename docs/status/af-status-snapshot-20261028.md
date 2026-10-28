# AF — management status and the pump mutex

`agent/devel-task-af`, measurement head `f570fb0cd`, base `486aa3a46`: gate NOTRUN (pending), comparison RUNNING. Evidence: docs/status/af-status-snapshot-20261028.md. Product change/defect: none claimed. Conditional snapshot decision waits for three repeats at1/10/50Hz. Host xorinox, existing real client untouched; no quiet-host claim.

## Purpose and source inspection

Task AF ([Stage 1 checkbox](alpha-1.0-2plus1-checklist.md)) asks whether
management polling serializes with the Raft pump enough to justify a bounded
published diagnostic snapshot. The change is conditional on a measured10Hz
pump-cycle or write-throughput effect outside the three-repeat spread.
Graph navigation seeded at `server_raft_host_status` identified callers and
Raft/KV reads; source inspection, not graph edges, establishes lock scope.

In base486aa3a46, status holds `h->mu` across group/peer collection, releases
it, then reads KV statistics and salt/root metadata. Those later reads are
already outside the mutex, unlike the older task description. No management
field, wire format, freshness contract or authoritative state path is changed
by this measurement commit. Task U is complete/merged; its prior D4 failure
and real-host IOR limitation are separate, not erased by AF.

## Reproduction and measurement scope

Assigned host xorinox, Linux,16 CPUs, private HOME filesystem, FUSE3.16.2.
The real UID1001 client234552 mounted at `/data1/efs/intel-mnt` is left
untouched. Environment and all-account process identity are retained with
the raw capture. These are shared-background measurements, not quiet-host
performance qualification. No real store/client/cluster is operated.

Export the committed source into `/home/maik/efs-task-af/run-3/candidate/src`,
create sibling screen/ with mode0700, then run detached:

```sh
SCREENDIR=/home/maik/efs-task-af/run-3/screen screen -dmS task-af-f570 \
  bash /home/maik/efs-task-af/run-3/candidate/src/scripts/af_campaign.sh \
  /home/maik/efs-task-af/run-3 /home/maik/efs-artifacts/af-20261028-f570fb0cd f570fb0cd
```

The campaign first copies an isolated measurement export, instruments only
its raft_host.c, and builds it. Product files in the clean export remain
untouched. SIGUSR2 toggles the load-window counters only on the exact new
private daemon PIDs. No production include, configuration reader, behavior
switch or management field is added. Both pump work (including mutex wait,
excluding idle poll) and entry-to-entry pump period (including idle poll)
have bounded64-bin logarithmic histograms and maxima. Status mutex wait and
hold have the same probes. Histogram quantiles are upper bounds; maxima are
exact clock differences. Probe clocks/atomics add overhead at every rate.
The existing EFS_RAFT_OBS hold/wait maxima include fixture setup as well as
load and are reported separately from the signaled probe window.

Nine fresh fixtures run in Latin-square order1/10/50,10/50/1,50/1/10Hz:
three repeats per rate. Each fixture uses three fault-capable daemons over
TCP/O_DIRECT with1GiB quota/member and one private FUSE mount. Four writers
use32 fixed names each and64KiB versioned deterministic D4 payloads, at five
operations/s/writer for60s. The D4 working-set cycle is O_TRUNC overwrite,
truncate-to-half then replacement, and in-place rewrite; each operation also
fsyncs. Packing is active (1s age/200ms pass), query refresh2s, KV memtables
256KiB. This is a **fixed-rate D4-derived diagnostic**, not full D4/soak
qualification. A separate stat loop runs at10Hz. Three independent pollers
request `raft-status` from all three members at the chosen rate and validate
both `leader_placement.parse_raft_status` and `qualify.parse_raft`.

All operation/parser/payload failures fail the trial; no retries or skipped
failed files. All128 final payloads must match their acknowledged versions;
raw pack PUT/read error counters must be zero. Actual throughput, missed
write/poll ticks, achieved polling counts, per-node status p50/p99, stat
p50/p99 and daemon CPU seconds are recorded. Sustained writes and polling
must reach80% of their offered rate, stat at least8Hz; failing those checks
is not a passing comparison. Status latency includes efs-mgmt process
launch/connection/formatting and is distinguished from server lock probes.

After the comparisons, the uninstrumented export builds and runs full
`make test-all` through the summary runner at8 jobs. The script preserves
gate-exit independently and propagates it to controller-exit. All gate
failures require untouched-base reproduction or a fix; host NOTRUNs stay
explicit. A snapshot and its term/leader/retired/freshness regressions are
needed only if the measured interference justifies that product change.

Raw evidence stays at xorinox
`/home/maik/efs-artifacts/af-20261028-f570fb0cd`; fixtures remain under the
private source parent's tmp/. Keep failures and investigation stores until
classified. Once reviewed, hash closed logs/results before pruning inactive
owned build/store/payload bulk with edelete; retain text evidence and
PRUNED.txt. Final medians/spreads, gate verdict, hashes and cleanup pending.

## Preserved first harness failure

Measurement-only f3e72d844 stopped with controller1 after the first1Hz trial:
the final raw pack-counter query used nonexistent `pack-status` and the CLI
rejected it. This is a harness command defect, not a passing trial or a
product error. The final128 payload checks had completed before that query;
no full comparison/gate verdict is claimed. Raw result.json,
logs and fixture `/home/maik/efs-task-af/measure/tmp/af-3wuv09uh` are retained
on xorinox for this failure. The correction uses the D4 driver's existing
`pack-compact SEED 0` summary-only command. It does not change product code,
workload, rate, deadlines or counter oracle. A fresh campaign uses a sibling
private run directory and new committed source/evidence identity; the first
failure is not overwritten.

Original failed12-file SHA256SUMS root: `f09fc77796873aba0124d2d51da904ce30812ff95936fc5774ff0f6edea68b9a`.

## Preserved offered-rate failure and capacity calibration

The corrected efe7894fe1Hz trial completed without application/parser errors,
verified128 exact payloads, and found zero raw pack PUT/read errors. It then
failed the sustained-rate check: writes391/390/393/392 against600 offered per
writer, with late ticks262/262/251/252. Controller1 and the failed capture
remain at `/home/maik/efs-artifacts/af-20261028-efe7894fe`; the private fixture
is retained. This is an **unsustained offered-rate baseline**, not a passing
trial and not evidence of a status-polling effect. No product code differs
between polling configurations. The fresh comparison calibrates its offered
rate to5 rewrites/s/writer (below the observed ~6.5/s baseline capacity),
keeping the same60s duration, payloads, operation mix, fsync, exactness and
zero-error oracles and the same80% achieved-rate check. The1/10/50Hz status
polling rates are unchanged. This calibration does not retrospectively pass
the failed10-writes/s trial. The harness now records all numeric measurements
before checking achieved rate so failed capacity diagnostics retain them too.

Second failed12-file SHA256SUMS root: `8d81256d9d58746f30c5c5a206c2e8ef3502a1063a5ce5d2b17ad2c7f14e2f35`; fixture `/home/maik/efs-task-af/run-2/measure/tmp/af-ih299h71` retained.
