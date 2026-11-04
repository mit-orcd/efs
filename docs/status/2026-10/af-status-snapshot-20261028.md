# AF — management status and the pump mutex

`agent/devel-task-af` tested `f570fb0cd`, base `486aa3a46`: TOTAL 545 FAIL 1 NOTRUN 11; same B6 slow EIO reproduced on untouched base. Evidence: docs/status/2026-10/af-status-snapshot-20261028.md. NOTRUN: cluster scripts/RDMA profile/sanitizer attach. Measured, no snapshot change. K35 availability defect unresolved; both failed fixtures retained.

## Verdict and scope

**AF complete as measured, no product change.** Three-repeat1/10/50Hz
comparisons do not demonstrate a10Hz pump-cycle or rewrite-throughput change
outside the baseline spread. A bounded cached status reply is therefore not
justified by the task's conditional trigger. Polling has CPU/stat-latency
costs on hardware; those findings are retained below. This is not a claim
that polling is free or that arbitrarily heavy polling is safe.

The clean full gate remains **FAIL**, with its sole failure reproduced on
untouched base486aa3a46. K35's degraded-fsync availability defect remains
unresolved and blocks broader acceptance; no AF change fixes or introduces
it. All30 completed performance trials pass their stated diagnostic oracles
(9 calibrated xorinox,3 strict50Hz,18 hardware TCP/RDMA). Full D4/F1/IOR,
multi-host performance and release acceptance are not claimed.

Source inspection, guided by a graph query seeded at
`server_raft_host_status`, finds `h->mu` held across group/peer collection,
then released before KV statistics and salt/root reads. The older task
text included those later reads inside the mutex. No src/include/Makefile/
test source differs from base486aa3a46. No status field, wire format,
version/age, freshness contract, authoritative path or product setting is
changed. Existing K33 and status-parser gates returned0; new cached-snapshot
transition tests are inapplicable because no cache was introduced. The graph
is navigation, not runtime correctness proof.

## Reproduction and identity

Assigned primary host: xorinox, Linux/16 CPUs/FUSE3.16.2, private HOME trees.
Its existing UID1001 FUSE client234552 on `/data1/efs/intel-mnt` was untouched;
primary measurements disclose shared background and make no quiet-host claim.
Task U is merged; its earlier failures/IOR limitations remain separate.

Export the named commit with git archive into ROOT/candidate/src; create
ROOT/screen mode0700 and run each campaign in detached screen. The scripts
pin the host/path, preserve exit receipts, logs and fixtures. Exact commands:

```sh
# Primary product/probe source f570fb0cd, nine balanced trials then clean full gate
bash scripts/evidence/af/af_campaign.sh /home/maik/efs-task-af/run-3 \
  /home/maik/efs-artifacts/af-20261028-f570fb0cd f570fb0cd
# Untouched git archive486aa3a46 in base-486/src; external tools at1d5c31e24
bash scripts/evidence/af/af_base_b6.sh /home/maik/efs-task-af/base-486/src \
  /home/maik/efs-artifacts/af-20261028-f570fb0cd \
  /home/maik/efs-artifacts/af-base-b6-20261028-486aa3a46
# After base reproduction closed; same instrumented binaries, corrected driver1d5c31e24
bash scripts/evidence/af/af_50_after.sh /home/maik/efs-task-af/run-3 \
  /home/maik/efs-artifacts/af-20261028-f570fb0cd \
  /home/maik/efs-artifacts/af-50hz-20261028-1d5c31e24 1d5c31e24
# Separate committed967db0570 source and private /tmp fixtures on fstor007
bash scripts/evidence/af/af_real_campaign.sh /tmp/efs-task-af/real-967db0570 \
  /data1/efs/evidence/af-real-20261028-967db0570 967db0570
# Aggregate a capture, or use --rates 50 for the strict supplement
python3 scripts/evidence/af/af_summarize.py CAPTURE
```

Clean xorinox: `efsd f570fb0cd`, branch agent/devel-task-af, built
2026-10-28T23:19:57Z. Its instrumented export reports "no git metadata", build
23:09:12Z; exact source f570fb0cd plus af_instrument.py/af_probe.h, compiler
flags, source hashes and binary hashes identify it. This overlay is never
used by the clean gate. The strict supplement reuses those binaries with
only its driver changed. Untouched base: `efsd 486aa3a46`, branch label
agent/devel-task-af-base, built23:38:04Z. Hardware: `efsd967db0570`, branch
agent/devel-task-af, built23:28:23Z on fstor007-mgmt. All times are Oct28UTC;
raw identity/version/build/hash receipts remain on the originating hosts.

## Diagnostic method and limits

Three co-located daemons, one FUSE mount,1GiB quota/member, TCP/O_DIRECT on
xorinox. Hardware repeats separately over TCP and mlx5_2 RDMA on
fstor007-mgmt (ibp193s0f0,172.16.4.150, PORT_ACTIVE), using the existing
private FUSE3.16.2 lib/rpath read-only. Node9901 was occupied by Task H/AD;
fstor007 staging began only after Task T exited and two all-account free-host
checks20s apart passed. No real cluster, client, store or37TB data was operated.

Each60s fresh fixture has four writers ×32 fixed names ×64KiB, versioned
D4 payloads,5 operations/s/writer. The D4-derived operation cycle is O_TRUNC
replacement, truncate-to-half then replacement, and in-place rewrite, each
with fsync. Packing1s age/200ms pass, query refresh2s,256KiB KV memtables,
and EFS_RAFT_OBS=1 keep background work active. A stat loop runs10Hz;
independent pollers query all three members. This is a fixed-rate diagnostic,
not full D4/soak verification. No operation error is retried or excluded;
all128 final acknowledged versions must match and both groups' raw pack
PUT/read error counters must be zero. Every measured reply is checked by
leader_placement.parse_raft_status and qualify.parse_raft.

Latin-square order1/10/50,10/50/1,50/1/10 gives three repeats/rate. Identical
bounded clock/atomic histograms measure pump work (including mutex wait,
excluding idle poll), entry-to-entry period (including idle poll), and status
mutex hold/wait. SIGUSR2 toggles only exact private daemon PIDs around load;
probe output is emitted at orderly exit. Quantiles are log2-bin upper bounds,
not exact percentiles; sub-bucket changes can be hidden. Printed Raft
high-water values cover setup too. Status RPC latency includes CLI launch,
connection and formatting; it is distinct from server mutex time. CPU is
measured per daemon, with role/leader variation between fresh fixtures.

Writers must sustain80% offered rate and stats at least8Hz. Initial pollers
discarded late ticks and required80% offered queries: their50Hz profile
achieved41.9–43.2Hz/member and is labelled **50Hz offered**. The separate
strict supplement and all hardware profiles use bounded synchronous catch-up
(one outstanding request/member, no retry) and require98% scheduled queries.
They each achieved exactly60/600/3000 replies/member at1/10/50Hz. Original
under-rate results remain intact; no failed gate is overwritten.

## Xorinox comparison


All nine f570fb0cd trials PASS the declared diagnostic oracles:128 exact
files/trial, no application/parser errors, zero raw pack PUT/read errors,
full sustained rewrite rate, and every member's four probe histograms present
at orderly stop. Three repeats at each rate; medians [minimum,maximum]:

| Metric | 1Hz/member | 10Hz/member | 50Hz offered/member |
|---|---:|---:|---:|
| Rewrite MiB/s | 1.249967 [1.249231,1.249974] | 1.248977 [1.248196,1.249981] | 1.249641 [1.248955,1.249830] |
| Total daemon CPU,s | 14.92 [14.80,14.97] | 14.70 [14.48,14.75] | 14.62 [14.27,15.13] |
| Stat p50,ms | 1.578 [.452,1.734] | .909 [.485,1.136] | .533 [.387,1.469] |
| Stat p99,ms | 45.036 [16.266,46.815] | 23.398 [12.193,50.716] | 15.479 [7.149,45.953] |
| Pump work p50 upper bound,µs | 16.383 [16.383,16.383] | 16.383 [16.383,16.383] | 8.191 [8.191,8.191] |
| Pump work p99 upper bound,ms | 8.389 [8.389,8.389] | 8.389 [8.389,8.389] | 8.389 [8.389,8.389] |
| Pump period p99 upper bound,ms | 8.389 [8.389,8.389] | 8.389 [8.389,8.389] | 8.389 [8.389,8.389] |
| Status mutex wait p99 upper bound,µs | 65.535 [65.535,8388.607] | 65.535 [32.767,16777.215] | 32.767 [32.767,32.767] |
| Status mutex hold p99 upper bound,µs | 4.095 [2.047,8.191] | 4.095 [4.095,4.095] | 4.095 [4.095,4.095] |
| Whole-fixture Raft hold high water,µs | 70339 [67684,81735] | 84749 [71343,105259] | 83167 [77548,116970] |
| Whole-fixture Raft wait high water,µs | 27209 [20736,40115] | 18218 [15555,37462] | 18202 [13883,19486] |
| All-member status queries | 180 [180,180] | 1800 [1800,1800] | 7623 [7546,7777] |
| Late write ticks | 0 [0,0] | 3 [0,4] | 2 [0,3] |

Per-daemon status latency includes process launch/connection/formatting. CPU
seconds and status p50/p99 in milliseconds, each median [min,max]:

| Hz | Member | CPU,s | Status p50,ms | Status p99,ms |
|---:|---:|---:|---:|---:|
| 1 | 1 | 7.05 [7.03,7.73] | 6.431 [6.393,6.456] | 69.442 [59.966,82.242] |
| 1 | 2 | 4.32 [3.52,4.35] | 6.225 [6.023,6.352] | 68.155 [52.678,113.045] |
| 1 | 3 | 3.55 [3.55,3.59] | 6.834 [6.017,7.290] | 63.118 [55.485,114.241] |
| 10 | 1 | 7.52 [3.83,7.67] | 2.807 [2.765,2.860] | 73.463 [55.720,76.493] |
| 10 | 2 | 3.54 [3.48,3.82] | 2.902 [2.389,3.084] | 63.116 [60.817,63.814] |
| 10 | 3 | 3.49 [3.48,7.10] | 2.834 [2.692,2.966] | 65.133 [53.747,65.361] |
| 50 offered | 1 | 7.29 [4.07,7.72] | 1.871 [1.851,1.887] | 65.022 [58.350,71.064] |
| 50 offered | 2 | 3.75 [3.52,7.00] | 1.888 [1.855,1.892] | 67.364 [59.547,74.624] |
| 50 offered | 3 | 3.55 [3.46,3.66] | 1.861 [1.827,1.864] | 66.992 [59.560,73.636] |


The10Hz throughput range overlaps1Hz (median0.079% lower), and pump work/
period p99 buckets are identical. No task trigger for a status cache is met.
The coarse buckets, short repeats and shared background limit extrapolation.

The stricter50Hz supplement completed9000 queries/trial in all three trials,
zero errors and128 exact files each. Median [min,max]: throughput1.249985
[1.249904,1.249987]MiB/s, total daemon CPU14.39[14.36,14.59]s, stat p50
.539[.500,.604]ms/p9911.408[9.306,13.716]ms, worst-member status p50
1.964[1.950,1.971]ms/p9965.948[65.409,66.749]ms. Pump work/period p99
upper bounds remain8.389ms in every repeat; status wait32.767µs and
hold4.095µs p99 upper bounds remain constant. Whole-fixture Raft hold max
71635[66984,83645]µs/wait14868[11394,21445]µs. These supplement the
original offered-rate profile, not the failed gate.

## Hardware confirmation

All18 cases completed with controller0,128 exact files each, no application/
parser errors and raw pack errors0/0. Every node emitted all four probes.
The117 ten-second ownership samples detect no foreign test/build work; no
background exception was used. This is sampled occupancy evidence, not proof
of absence of every transient between samples. Nine RDMA trials record
66–84 connections/trial, all TCP-counter measured, positive app_tx+app_rx
endpoint totals948580–1025871, zero post-setup TCP bytes and zero violations.
These are endpoint counters, not unique wire-byte throughput. Hardware
measurements closed before independent review/hash/pruning.

Medians [min,max], three repeats per transport/rate:

| TCP metric | 1Hz | 10Hz | 50Hz |
|---|---:|---:|---:|
| Rewrite MiB/s | 1.249984 [1.241710,1.249987] | 1.249983 [1.249982,1.249986] | 1.249987 [1.249985,1.249988] |
| Daemon CPU total,s | 11.50 [11.43,11.57] | 12.47 [12.18,12.72] | 13.28 [13.12,13.35] |
| Stat p50,ms | .548 [.429,.583] | 2.691 [2.148,2.945] | 2.205 [1.935,2.373] |
| Stat p99,ms | 5.078 [5.039,5.179] | 23.202 [7.421,41.155] | 7.368 [6.520,7.949] |
| Worst-node status p50,ms | 11.280 [10.725,11.532] | 8.335 [8.277,8.412] | 5.779 [5.760,5.869] |
| Worst-node status p99,ms | 19.188 [18.846,596.559] | 68.124 [20.649,69.601] | 18.471 [18.162,18.478] |
| Pump work p99 upper,µs | 131.071 [131.071,262.143] | 262.143 [131.071,262.143] | 131.071 [131.071,262.143] |
| Pump period p99 upper,ms | 8.389 [8.389,8.389] | 8.389 [8.389,8.389] | 8.389 [8.389,8.389] |
| Status wait p99 upper,µs | 131.071 [32.767,262.143] | 65.535 [32.767,65.535] | 1.023 [1.023,1.023] |
| Status hold p99 upper,µs | 8.191 [8.191,8.191] | 8.191 [8.191,8.191] | 8.191 [8.191,8.191] |
| Whole-fixture Raft hold max,µs | 3805 [2939,33928] | 2971 [2849,3319] | 2717 [2673,2721] |
| Whole-fixture Raft wait max,µs | 657 [557,658] | 714 [670,1202] | 919 [859,969] |

| RDMA metric | 1Hz | 10Hz | 50Hz |
|---|---:|---:|---:|
| Rewrite MiB/s | 1.249984 [1.249981,1.249986] | 1.249984 [1.249984,1.249986] | 1.249989 [1.249988,1.249989] |
| Daemon CPU total,s | 14.43 [14.42,14.71] | 15.38 [15.14,15.57] | 16.59 [16.56,17.33] |
| Stat p50,ms | .514 [.499,.521] | 1.989 [1.956,2.189] | 2.162 [2.101,2.578] |
| Stat p99,ms | 5.204 [4.984,5.386] | 6.985 [6.559,7.468] | 6.934 [6.546,44.604] |
| Worst-node status p50,ms | 11.666 [11.579,12.256] | 8.314 [8.245,8.345] | 5.862 [5.847,5.920] |
| Worst-node status p99,ms | 19.952 [19.948,21.288] | 19.781 [19.727,19.860] | 17.880 [17.466,17.882] |
| Pump work p99 upper,µs | 131.071 [131.071,262.143] | 262.143 [131.071,262.143] | 131.071 [131.071,262.143] |
| Pump period p99 upper,ms | 8.389 [8.389,8.389] | 8.389 [8.389,8.389] | 8.389 [8.389,8.389] |
| Status wait p99 upper,µs | 131.071 [65.535,1048.575] | 32.767 [32.767,65.535] | 2.047 [1.023,8.191] |
| Status hold p99 upper,µs | 8.191 [8.191,8.191] | 8.191 [8.191,8.191] | 8.191 [8.191,8.191] |
| Whole-fixture Raft hold max,µs | 2873 [2716,2881] | 3216 [3048,3502] | 3170 [3168,4081] |
| Whole-fixture Raft wait max,µs | 564 [548,1372] | 644 [617,663] | 768 [703,776] |

Hardware10Hz CPU and stat latency increase outside1Hz ranges; polling has a
measurable cost. Pump work/period and throughput still overlap baseline.
CLI launch/burst phasing and non-mutex status work remain confounders: this
does not establish that caching under the pump would remove the cost. The
1Hz TCP status-tail outlier596.559ms and50Hz RDMA stat-tail outlier44.604ms
remain in the spread. Raw JSON retains per-member CPU/latency/counts.

## Failed evidence and gate exception

- f3e72d844: first trial stopped because the harness queried nonexistent
  pack-status. Controller1, original capture preserved. Corrected to D4's
  existing pack-compact SEED 0 summary command; no product/oracle change.
- efe7894fe: baseline offered10 writes/s/writer sustained only391/390/393/392
  of600, failed the80% rate oracle. Payload/error checks had passed; it is
  still a failed capacity diagnostic. The fresh rate5 calibration is below
  observed~6.5/s capacity; same operations, fsync,60s, payload/error oracles.
  Neither earlier failure is counted as a passing trial. Their classified
  scratch bulk was pruned only after raw logs/results were hashed.
- Clean gate f570fb0cd: `TOTAL545 FAIL1 NOTRUN11 STATUS FAIL (926s)
  OVERBUDGET14 BUILD11s`; controller/gate1/1. Sole failure case481:
  EFS_B6_MODES=slow python3 tests/test_b6_unavailable.py, line402, F0 fsync
  EIO60.07s, other six fsyncs8.01s. At23:28:52.882Z the private client logs
  fragmentsDPD, "sweep deadline exhausted; obligations retained", then EIO.
- Untouched486aa3a46 on xorinox reproduces the same command/assertion:
  F0 EIO60.02s, remaining six8.01–8.06s, test/controller1/1. Source inspection
  is consistent with199ad2afd's shared60s COMMIT/proof budget exhausting
  before a required degraded mark. Causal attribution to that commit is
  not proven by a revert test. This is K35 availability, not permission to
  ignore a failed COMMIT or relax proof/deadline/durability requirements.

The task's actual-base exception is satisfied, but the raw gate stays FAIL.
Eleven NOTRUNs: absent cluster scripts, declaration RDMA frame path plus eight
native commands unavailable in the forced-TCP profile, and precise sanitizer
attach refusal. All other timing receipts return0; NOTRUN is not promoted
to coverage. Fourteen advisory budget overruns remain in summary/timings.

**Open K35 investigations remain inactive on xorinox**, including original
logs/stores and binaries:
`/home/maik/efs-task-af/run-3/candidate/src/tmp/case-481/efs-b6-nvuie2ds` and
`/home/maik/efs-task-af/base-486/src/tmp/efs-b6-9lxmg_6t`.
These are intentionally not deletion candidates. Existing user clusters,
clients, mounts and stores were never stopped or cleaned up.

## Preservation and validation

Raw locations and FINAL_SHA256SUMS roots (all text/JSON/logs retained):

| Origin directory | Files | Manifest SHA256 |
|---|---:|---|
| xorinox:~/efs-artifacts/af-20261028-f3e72d844 | 15 | 4d7ecc5e8f1556d7de1144d01c407da23758403e11971723eae8c65af7b2833e |
| xorinox:~/efs-artifacts/af-20261028-efe7894fe | 15 | 77de832d4696cf54ae98e9efbe6bd62a4d0a0ee446c5dae639531290fa0ec93f |
| xorinox:~/efs-artifacts/af-20261028-f570fb0cd | 94 | dc20b3d8d9a1fed8dfea825d30d212dc15b71073865d874d76565b23d64568a9 |
| xorinox:~/efs-artifacts/af-base-b6-20261028-486aa3a46 | 19 | daaa8845f2cc0649d1b0a311b528f1fd67905e8de0d02e873a2708202e90b2a0 |
| xorinox:~/efs-artifacts/af-50hz-20261028-1d5c31e24 | 29 | fe7768482870537e66f05b91d5e59f8e882f063574d33e7c1536741489db0a3a |
| fstor007:/data1/efs/evidence/af-real-20261028-967db0570 | 140 | ee231a7918ca3028e6441798e782ba0bbe05b679151b7ed86cb2c7aafa9e08e7 |

Capture hashes were computed and rechecked before edelete. PRUNED.txt lists
exact removed paths: classified harness/calibration and successful measurement
source/build/store/payload bulk on xorinox, and only the successful private
real-967db0570 tree on fstor007 (errors0). Both K35 investigation roots remain.
No private daemons or mounts remained before pruning. Raw evidence directories
and original failure manifests are retained; no raw artifact/payload enters Git.
Reproduction uses committed scripts and fresh fixtures, not private captures.

Documentation generation and all five checks, canonical graph rebuild and
nine graph tests pass. Parser warnings/unresolved edges are coverage gaps;
graph edges do not prove runtime behavior. No merge or push; the coordinator
owns the inherited K35 fix and merged qualification.


Subsequent [K35 slow-fsync fix and qualification](k35-slow-fsync-20261029.md)
uses product `1ab9fef83`. AF's original failed gate and untouched-base
reproduction remain failures; both fixtures and their evidence are unchanged.
