# W90 / D33 — readable durations and large expiry backlog

**PASS, October 12–13, 2026 (America/New_York):** new policy arguments, 24/24
expiry lifecycle gates, three 10,000-file backlog phases, core POSIX 216/216
(24.1s; no fail/skip/notrun), peer 67/67 (12.4s; no failures), ordinary physical
GC, cold empty fragment inventories, drained queues, zero quotas and late-PUT
fences. Harness exit 0. This qualifies finite correctness; the scheduling limit
and rising sampled RSS below remain performance/capacity follow-ups.

## Policy arguments

```sh
efs-mgmt expiry-policy NODE:PORT set ROOT_INO GENERATION EXPECTED_REV \
  --expire-after 24h --renewal-limit 7d --flags 3
```

`--expire-after` sets initial expiry relative to creation. Changes affect future
files; existing deadlines remain unchanged. `--renewal-limit` bounds a renewed
deadline relative to renewal time. Renewal checks the current limits/permissions
at the file's saved policy source. Repeated renewal may extend lifetime
indefinitely. Reads/writes do not renew. The omitted renewal limit defaults to
expire-after and must be at least that duration. This adds no absolute lifetime cap.

Durations are positive integers followed by ns, s, m, h or d. Fractions, missing
units, overflow, duplicate/unknown arguments and invalid limits are rejected.
FLAGS retains existing semantics: bit 0 enables defaults, bit 1 permits owner
clearing. Set requires operator UID 0 and EFS_EXPIRY=1. The positional legacy
TTL_NS/MAX_NS/FLAGS form remains supported. Normal subcommand help explains the
options and semantics without opening a connection.

## Tested source and configuration

Live/native candidate: `656fce08f37bb48edaa0a0ef78e9948c45a2f2b8` on
`devel-expiry`; clean Git archive of build/test inputs. Later
`5bd5a95ca83c71467e717dbac9dcb9fd4809c6fc` only clarifies help text and its
assertion: current source-policy limits apply to later renewals. Duration parsing,
wire requests, storage/server code and live scripts are unchanged. Its native
management build passes duration/help/wire compatibility and GC-status regressions;
no second long live run is claimed for that help-only change.

NUC Intel i5-1135G7, four cores/eight threads, x86_64 Linux
6.12.0-211.64.1.el10_2.x86_64; /data1=/dev/sda XFS, rw/noatime. Four private
nodes with 1 GiB quota each, two Raft groups. Primary FUSE mount during backlog;
second mount for peer qualification. TCP/direct-I/O/FILES; EFS_COMMANDS=0,
EFS_COMMANDS_SERVER=0. EFS_EXPIRY=1
except the deliberate global disabled phase. Actual commands/environments and
mapped executable hashes are retained in expiry-runtime.json. About 204 GiB free
at start. Existing stores and the primary devel checkout were untouched.

## Reproduction

Linux, GCC/libibverbs/FUSE3/Python3 available; both approved mount paths empty
and unmounted, four free ports, private storage parent. Clean native build:

```sh
make -j4 EFS_FAULTS=0 EFS_GIT_ID=656fce08 \
  EFS_GIT_DESCRIBE=656fce08 EFS_GIT_BRANCH=devel-expiry \
  efsd efs-fuse efs-mgmt tests/test_expiry tests/test_meta_apply
./tests/test_expiry
./tests/test_meta_apply
python3 tests/test_gc_status_expiry.py ./efs-mgmt
python3 tests/test_expiry_cli.py ./efs-mgmt
bash tests/run_tests.sh posix expiry /data1/efs/mnt \
  --root /data1/efs/evidence/expiry-656fce08-backlog \
  --peer-mount /data1/efs/mnt2 --port 24410 --jobs 4 --backlog-files 10000
```

Native flags: O3, debug/frame pointers, march/mtune=native, EFS_FAULTS=0.
`tests/live/expiry_backlog.py` defines the workload; gc_reclamation.py owns starts,
stops and fresh stores. Each phase creates/fsyncs 10,000 distinct 1-KiB files with
eight workers. A one-hour birth policy is materialized, then explicit renewal
schedules deadlines 180s ahead; staggered bins differ by 10s. Queue preparation
must finish at least 30s before the first deadline. A renewed seven-byte control
has a two-hour deadline. Foreground probes repeatedly create/write 1000 bytes/
fsync/read/verify/unlink outside the expiring tree, paced by 100ms between cycles.
Ten-second baseline precedes arming; probes continue throughout measured drain.

Restart phase first requires some deletion and some pending work, then unmounts
with no foreground writes pending, kills/restarts the private servers and remounts.
Disabled phase restarts with EFS_EXPIRY=0 before deadlines, waits five seconds
past them, verifies all 10,000 files plus control and all four disabled statuses,
then restarts with EFS_EXPIRY=1. Every phase requires namespace and exact per-inode
fragment absence, verifies control bytes, performs another cold restart and checks
that only the control remains before cleanup. Six backlog-specific restarts total.

Namespace drain bound = max(1800s, count/4): 2500s for 10,000 files. Physical drain
gets 300s more; every approximately 2s sample checks free space >2 GiB and daemon
RSS <512 MiB. Progress and observations are saved incrementally. Larger accepted
argument counts are not a qualification claim; quota, preparation and control
lifetime bounds need review before using them. Core/peer/ordinary-GC gates follow.

## Results

| Phase (10,000 files each) | Creation | Measured namespace drain | Physical empty from measurement start | Foreground p95 baseline → drain | Worst foreground cycle |
|---|---:|---:|---:|---:|---:|
| Staggered deadlines |66.9s|1398.0s (23.3min)|1398.6s|20.0 →26.1ms|81.9ms|
| Mid-cleanup restart |62.3s|1329.6s (22.2min)|1330.3s|19.8 →25.9ms|75.0ms|
| Global disable/re-enable |64.1s|1334.8s (22.2min)|1335.5s|23.6 →26.4ms|123.9ms|

Restart had 9,992 files pending immediately before interruption. Disabled mode
preserved all 10,000 overdue files. Both resumed and completely drained, with
control bytes intact after cold restart. For restart/off-on, drain measurement
starts after remount; deadline-to-observed-empty was 1335.3s/1344.8s respectively.
Staggered drain starts at first deadline; last-bin deadline-to-empty was 1368.1s.

Foreground cycle samples: 11,534 /10,962 /10,963 during drains; baseline 85 /85 /83.
Median baseline→drain: 18.45→20.74ms /18.70→21.26ms /21.00→21.67ms. These are whole
operation cycles, not individual fsync latency. Creation timing includes fsync
and expiry-record reads; it is not comparable to a zero-payload generator rate.
All four end-of-drain statuses reported expiry_errors=0 in each phase. Counters
are process-local and restart-reset; they are not a lifetime ledger.

Resource samples:688 /653 /654. Maximum sampled daemon RSS rose across phases:
189,376 /269,024 /401,620KiB (185 /263 /392MiB). Minimum observed filesystem free
space 212,102,651,904 bytes (~197.5GiB). End free inodes 368,234,659. Samples can miss
brief peaks. Guard PASS does **not** prove steady-state bounded total RSS: the
rising peaks need a longer churn/retained-metadata investigation and comparison
before attributing them to expiry or claiming a memory plateau. Foreground probes
added 33,459 create/delete cycles alongside the 30,000 expiring files.

### Default-worker scheduling limit

`host_gc_expiry_pass` attempts at most eight operations/group/pass, checking a
50 ms budget between operations; the normal GC loop normally sleeps 1000 ms.
This flat-directory workload concentrates metadata in one group. Observed
roughly 7–7.5 deletions/s is consistent with that software scheduling ceiling.
No hardware ceiling is established. Deadlines mark eligibility for asynchronous
cleanup; files remain visible until the worker removes them. Large queues can
therefore stay overdue for many minutes. A configurable/fair progressing-expiry
cadence is a future performance question, not implemented or qualified here.

The initial bd6570a9 compile failed on a duration parser/formatter name collision,
fixed by 6b3f85ed before deployment. Preliminary 6b3f85ed/d7bd5b1d live runs passed
24 lifecycle gates but were deliberately interrupted before backlog acceptance;
the latter revealed the too-short 600s test assumption. 656fce08 lengthens the
fixture bounds/control lifetime and checkpoints measurements, without changing
worker budgets. Retained preliminary diagnostic:
/home/efs/expiry-d7bd5b1d-throttle-observation.txt. Neither interruption is a PASS.
Runner routing/configuration, shell syntax and NUC POSIX runner self-test PASS.
macOS self-test hit a child-PID-file timing race; no macOS qualification claimed.

## Evidence and cleanup

On NUC: build /home/efs/devel-expiry-656fce08-normal; log
/home/efs/expiry-656fce08-backlog.log; build log /home/efs/expiry-656fce08-build.log.
Fixture parents /data1/efs/evidence/expiry-656fce08-backlog/gc-direct-fjz6q4m8 (n1/n2,
reports) and gc-direct-avjtxxv_ (n3/n4). Both mounts and all owned daemons stopped;
final cold inventories empty, queues drained, quotas zero. Existing data preserved.
Raw logs, stores and measurements remain outside Git. Inspect retained evidence
before removing only these generated private fixtures; do not wipe user stores.
This run did not replace /data1/efs/src or permanently start the regular cluster.

| Artifact | SHA256 |
|---|---|
| efsd (656fce08) |59118ccddef8e210663b790701d547fc372626ece48be8c08335a754b36b8e5e|
| efs-fuse (656fce08) |b27b688ef51d9c24c2326e88b90f58e4709d6a658fef6a9648b842dafbcd0dc9|
| efs-mgmt (656fce08) |518437b47d66570dcb45e9472deb0eff80d1cb515ea39bf8dfe02029f0955b83|
| source archive /home/efs/expiry-656fce08-code.tar |2fad0ad57d211b8960494460aebaac0e6801d3b62ba723e210897eea8f75bc6f|
| run log |8faef6cd7108c27316d085ad6924b3bc3c409b868255e72fccbfa138ed3093e0|
| build log |1ea56b2baa259af80dadbf131fec2dabd1dce8a61c2621cf4927658c4bf399dc|
| manifest.json |5fe8a8dd60efe454704b5ef361f69e8e72079d0d55e1d76ee69f5293bb187829|
| expiry-runtime.json |b1a55b673ce83a2893b76273195aeed4f6079f33b9cbf5a2b443b3c39e46b268|
| expiry-lifecycle.json |9834ae464b41b881d9d2ff436ae87ca5b5842f14613ba856ed9a5a261e7a78b4|
| expiry-backlog.json |b7f892b4fbc4cc223d4285396c1fa2407a856f072c3c17f2bcc200e3cbfe5d7b|
| posix.tsv |8b28617347ef665f46aa47323b439869a1757c30f68a0241e25983c4288b3714|
| posix2.tsv |015597261b3c7c120476b8a4bafcf14d5f06714001475b5451460fbe81a59526|
| final help-only efs-mgmt (5bd5a95c) |430e6883e0820396c42d594365b648fae7ca49fc606528c57cd05ba6f47bac67|
| help-only build log |0238eed7c80be013bc235a633fec6d9b0f9799ad76e68e1b383103cdb91ecace|

Help-only build /home/efs/devel-expiry-5bd5a95c-cli, with same native flags and
build IDs 5bd5a95c. Reproduce with make efs-mgmt and both Python CLI regressions
above. Its build log is /home/efs/expiry-5bd5a95c-cli-build.log. The matching
byte-level legacy/named request regression PASS confirms unchanged serialization.

## October 13 performance follow-up

The historical results above are preserved. Retained-history RSS and cleanup
cadence are addressed on `devel-expiry` in
[the matched-log and new backlog report](expiry-rss-cleanup-20261013.md): all
three 10,000-file phases pass with 212–218 second drains and 60–72 MiB sampled
node RSS, with a measured foreground tail-latency tradeoff.
