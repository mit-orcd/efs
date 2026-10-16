# W90 — foreground latency during fast expiry cleanup

Verdict: PASS on `devel-expiry`. Qualified implementation
`99f6ef0cfa8bdda5e91a87bbd358919b6e11fe3b`: native regressions, all 30,000
backlog files, normal and clock-fault expiry/POSIX/peer/cold matrices PASS.
Measured foreground p95 improves 20–40% with physical cleanup 25–29% faster.

## Investigation and change

[The preceding RSS/cleanup fix](expiry-rss-cleanup-20261013.md) reduced each
10,000-file queue from 22–23 minutes to 212–218 seconds, but foreground cleanup
p95 was 29.156 / 32.892 / 33.019 ms versus pre-cleanup 21.750 / 21.761 / 22.112 ms.

Code inspection found that progressing expiry shortened the entire GC loop's
sleep to 100 ms. This also multiplied close/orphan/fragment scanning and
transaction recovery, even though only expiry requested that cadence. Previous
n1/n4 logs recorded 3,327/1,650 full GC passes; all n1 pass recovery totals
20,186 ms and fragment totals 5,384 ms across the run. Those are aggregate
work measurements, not exclusive CPU time or proof of the sole p95 cause.

`e98ac96f` separates the schedules: full GC preserves its ordinary/backlog policy;
expiry-only passes run during the yield while successful deletion proves that
more due work remains. `d5b23c38` uses the existing monotonic iostats clock for
cadence deadlines, keeping wall-clock changes separate from eligibility.
The initial e98ac96f lifecycle run was deliberately interrupted before the
large backlog test to deploy the monotonic correction; it is NOT a qualified
complete run. Its owned fixture cleaned up, with both mounts unmounted and no
daemons remaining. No pending writes were forcibly discarded.

The first completed d5b23c38 staggered experiment is rejected as a solution:
namespace 176.976s, physical 343.712s; foreground baseline p95 21.110ms and
cleanup p95 28.552ms, median 19.632ms, max 73.748ms (1,483 cleanup samples).
Physical debt remained after namespace removal: a capped 32-marker reap page,
no fragment pending estimate, and ~47 MB data still allocated on sampled n3.
The 100 ms namespace cadence alone cannot replace fast reap processing.
Its first phase passed physical/cold checks, then its remaining experiment was
interrupted through owned-harness cleanup; it is NOT a complete 30,000-file gate.
Evidence `/data1/efs/expiry-d5b23c38-backlog/gc-direct-gylnyg93`, run log
`/home/efs/expiry-d5b23c38-backlog.log`.

Refined candidate `99f6ef0c` retains fast reap/fragment work alongside expiry.
Close-step/close/orphan discovery, directory spread and transaction recovery use
an independent one-second maintenance deadline. Expiry uses at most two files
per pass, rather than eight adjacent transactions, with additional two-file
passes spaced by 20 ms while work makes progress. The existing 50 ms
between-operation ceiling remains; slow individual operations can exceed it.
Both maintenance and yield deadlines use the monotonic clock. Eligibility
continues to use the separate guarded wall clock.

Each extra expiry pass rechecks hosted groups and leadership, then uses the
existing quorum/clock/conditional-delete path. No progress stops fast retries
until the next ordinary pass. Necessary physical GC still runs between yield
windows. Expiry time counts toward the yield; shutdown/snapshot checks retain
at most 20 ms sleep slices, with existing work budgets. GC pass counters count
reap/fragment cycles; expiry counters include the extra passes. No sync,
persistence, cache, public request or eligibility rule changes. A successful
result requires BOTH latency improvement and preserved physical cleanup speed.

## Qualification and reproduction

Same NUC Intel i5-1135G7, four cores/eight threads, Linux
6.12.0-211.64.1.el10_2.x86_64; `/data1` SATA XFS. Four private fresh 1 GiB-quota
nodes, two Raft groups, mounts `/data1/efs/mnt` and `/data1/efs/mnt2`, TCP/direct
I/O/FILES. EFS_COMMANDS=0, EFS_COMMANDS_SERVER=0;
EFS_EXPIRY=1 except deliberate disabled phases.
Both mount paths must initially be empty and unmounted, approved by FUSE policy;
ports 24410–24413 must be free. Never attach this destructive fixture lifecycle
to user stores.

Native candidate build `/home/efs/devel-expiry-99f6ef0c-normal`; source archive
`/home/efs/expiry-99f6ef0c-code.tar` contains tracked build/test sources from the
exact commit. Build/native/live logs `/home/efs/expiry-99f6ef0c-{build,native,backlog}.log`.

```sh
make -j4 EFS_FAULTS=0 EFS_GIT_ID=99f6ef0c \
  EFS_GIT_DESCRIBE=99f6ef0c EFS_GIT_BRANCH=devel-expiry \
  efsd efs-fuse efs-mgmt tests/test_raft_store tests/test_raft \
  tests/test_kv_lsm tests/test_expiry tests/test_meta_apply
./tests/test_raft_store
./tests/test_raft
./tests/test_kv_lsm
./tests/test_expiry
./tests/test_meta_apply
python3 tests/test_gc_backlog_idle.py
python3 tests/test_expiry_cli.py ./efs-mgmt
python3 tests/test_gc_status_expiry.py ./efs-mgmt
bash tests/run_tests.sh posix expiry /data1/efs/mnt \
  --root /data1/efs/expiry-99f6ef0c-backlog \
  --peer-mount /data1/efs/mnt2 --port 24410 --jobs 4 --backlog-files 10000
```

Native GCC O3, debug/frame pointers, march/mtune=native. Same committed workload
as afbce294: three 10,000-file phases, eight creators, fsynced 1 KiB payloads,
180-second deadline lead, staggered deadlines, mid-drain restart, disabled
preservation and re-enable, renewed controls, independent physical/cold checks,
foreground create/write/fsync/read/unlink cycles paced by 100 ms. Drain bounds
and RSS/free-space guards unchanged. No profiler runs during timing samples.
The fixture also runs full POSIX and peer suites and final cold queue/quota/
late-PUT checks. Harnesses stop owned mounts/daemons and retain private evidence.
Raw artifacts stay outside Git.

## Results

Refined staggered phase PASS: creation 64.274s, namespace 151.916s, physical
152.671s. Foreground baseline median/p95/max 19.179/20.060/25.206ms (84 samples);
cleanup median/p95/max 13.674/23.327/59.539ms (1,321 samples). Against afbce294,
p95 29.156→23.327ms (20.0% lower); increase from each run's own baseline
7.406→3.267ms (55.9% smaller). Physical drain 216.298→152.671s (29.4% shorter).
Restart phase PASS: creation 75.964s, 9,994 pending before restart, namespace
158.137s, physical 158.893s. Baseline median/p95/max 20.835/23.101/32.830ms
(83 samples); cleanup 12.808/19.726/249.105ms (1,388 samples). Prior fast run
p95 32.892ms and max 182.228ms: p95 improves 40.0%, but maximum worsens.
New cleanup p95 is below its pre-cleanup baseline. This measurement does not
establish why; it is not a claim of a universal foreground speedup. Physical cleanup is 25.2% shorter.

Off/on phase PASS: creation 77.994s, all 10,000 overdue files preserved disabled,
namespace 164.132s, physical 164.953s. Baseline median/p95/max
20.855/22.819/26.521ms (84 samples); cleanup 13.359/21.244/246.495ms
(1,434 samples). Prior p95 33.019ms: 35.7% lower; physical cleanup 24.6% shorter.

| Phase | Prior cleanup p95 ms | New p95 ms | Prior physical seconds | New physical seconds |
|---|---:|---:|---:|---:|
| staggered | 29.156 | 23.327 | 216.298 | 152.671 |
| restart | 32.892 | 19.726 | 212.494 | 158.893 |
| off/on | 33.019 | 21.244 | 218.831 | 164.953 |

All 30,000-file phases PASS, renewed controls, physical fragments and cold
inventories verified independently. Sampled peak RSS KiB 74,388 / 64,428 /
77,956 (maximum 76.1 MiB, versus prior 72.2 MiB). This is slightly higher,
not an additional memory reduction; all guards pass. No nonzero sampled
expiry/GC error counters. Creation is mixed: 64.274/75.964/77.994s versus
72.264/72.343/73.509s previously. Broad throughput improvement is not claimed.
Normal expiry 24/24, POSIX 216/216 (26.2s), peer 67/67 (12.6s), final cold
physical/queue/quota/fencing gates PASS. Clock-fault 25/25, POSIX 216/216 and peer 67/67 PASS.
Sequential runs are not a randomized repeated A/B trial or a hardware ceiling.

Normal primary evidence `/data1/efs/expiry-99f6ef0c-backlog/gc-direct-pbz2ikop`.
Mapped executable/environment records verify actual transport, direct-I/O and
feature settings; logs and JSON/TSV remain outside Git.

| Normal artifact | SHA256 |
|---|---|
| efsd | 90766efd633c2d1e480f2c8e97158fca5bc819fac867531925db9ad55a2645dc |
| efs-fuse | 7fa625e4b149ab646483ff889eee9096bc4fd17fd6438a31f8ead0fa9d0888ea |
| efs-mgmt | eb3fefcdf022b362ed2dfee2e4a28cad864fdeea890b8381265945e5b2c8d4fc |
| source archive | cd2c64e2d5fb1dab0e8b19bcdcfc9a16d5e5b25cb7ecb1756657ee9d86b12d70 |
| run log | a7b6a9c5e56121fdefca8ecc345c96218e47dba7b909d88b468e4e37b8fb931a |
| native log | b4af6725bf6ae9a357696b7f959f91c2ff76a70c11037ab5b9393f53787d77fa |
| manifest.json | 55a0e45788b2d24c4447b4b4df2bbde6e02af296c21b7a798169b7dd41236d34 |
| expiry-runtime.json | 1a71bd9af889c45a0d8d942bf08f6c9a3416ddae33fac1e1c82cf7714eb7bf1f |
| expiry-lifecycle.json | 6c0b9770c2d3092a9b9be90d9090f3b95961fce9a89aa40ff6e7bab0a0cdeb6a |
| expiry-backlog.json | 5bd246e789f8644bcef77e9e9532c6b79a83a56b96e119040ac078162762fe33 |
| posix.tsv | e092a410f86ae4169dd824100e178ce22cea5301ad1fb2ef141d8ce4ff7fa0ac |
| posix2.tsv | 7ff9e81eaaa177c166ca7bb137d3d3b182cba7d9926737eeb9ed6bd7747cf33d |

Clock-fault reproduction from a separate clean source extraction:

```sh
make -j4 EFS_FAULTS=1 EFS_GIT_ID=99f6ef0c \
  EFS_GIT_DESCRIBE=99f6ef0c EFS_GIT_BRANCH=devel-expiry efsd efs-fuse efs-mgmt
bash tests/run_tests.sh posix expiry /data1/efs/mnt \
  --root /data1/efs/expiry-99f6ef0c-fault \
  --peer-mount /data1/efs/mnt2 --port 24410 --jobs 4 --clock-fault
```

Clean build `/home/efs/devel-expiry-99f6ef0c-fault`; build/run logs
`/home/efs/expiry-99f6ef0c-fault-build.log` and `/home/efs/expiry-99f6ef0c-fault.log`.
Same host/configuration. Fault markers inject clock health, not the host clock.
Fault aggregate PASS: expiry 25/25, core 216/216 (23.8s), peer 67/67 (12.3s),
final physical inventory, queues, zero quota and late-PUT fencing. Both normal
and fault commands exited zero. Primary fault evidence
`/data1/efs/expiry-99f6ef0c-fault/gc-direct-x44el00x`.
Final cleanup verified both mounts unmounted and no efsd processes remaining.
Existing user stores preserved; all owned raw evidence retained outside Git.
Normal RSS/free-space samples 76/79/82; minimum free bytes 207,470,972,928.

| Fault artifact | SHA256 |
|---|---|
| efsd | ac3a293d198a2362475febce9f4967e741cfe05b21ebc17994c3015ae9406d2f |
| efs-fuse | 5c33a4b4f0c4a34e208333e685bb97a2a159ac2200d26f0f0a6fcd19615b2382 |
| efs-mgmt | 66d1d76af8fef6fb6c891b3cbd1c69dc76abd2dcebbcb99ccf13cc79843fef88 |
| run log | 3e07220826c70f055491da53ac2afa38af3c49f62a0b492d7d354b51049e3fe9 |
| manifest.json | d10f6ab87a8d391cbc368a404eb1191067fdc18b6c79bee65a3699b307dae825 |
| expiry-runtime.json | 5a70f7cf196ffc7e4d46e0c3d420e580d5ff18bfdec1d6d489583dfea4f0714b |
| expiry-lifecycle.json | 98f4e0e71e6bc3edaf2faec0c061175f9073c158072fdecc8d20c52e103abb2f |
| posix.tsv | eb3c9261a3c8734b29495f8e26709104d1e6f0f58fb2e45bd2c31872f2464e2d |
| posix2.tsv | 2316869ae205ff275edaf3cdcbd1ce1e937f7d93dec28269e415528c52631b04 |
