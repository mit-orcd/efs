# W90 — expiry cleanup cadence and retained-history RSS

Verdict: PASS on `devel-expiry`. Native/failure/sanitizer regressions, matched-log
heap investigation, 30,000-file backlog, normal and clock-fault live matrices
complete. Qualified implementation commit
`afbce2948e228c8506b52f09f272374a2d77e32a`.

## Findings and implementation

The previous 30,000-file run (`656fce08`) drained 10,000 files in 22–23 minutes,
with sampled daemon RSS peaks 185 → 263 → 392 MiB. Its foreground probes added 33,459
create/delete cycles. Profiling must distinguish churn history from a leak.

A native probe on a private copy of that run's n3 Raft log found 2,100,419 retained
entries, 248,510,144 bytes of command copies and 125,829,120 bytes of index allocation.
Both groups had snapshot index 0: the existing 512 MiB command-byte snapshot trigger
had not fired. Generic-x86 Massif attributes its peak mainly to
raft_group_put_entry and raft_group_reserve, confirming history retention rather
than assigning the growth to the disabled .efs catalog or assuming an expiry leak.

Candidate `afbce2948e228c8506b52f09f272374a2d77e32a`:

- Preserve the complete authoritative log, framing/CRC, snapshot retention window,
  sync ordering and ACK guarantees. Command copies become a 2 MiB/group cache.
  Eviction does not delete a log entry; misses read its original frame and verify
  checksum, group, index, term and size. Rotation reads evicted commands from the
  old fd and publishes replacement offsets only after fsync/rename succeeds.
- On Linux, entry indices use private unlinked MAP_SHARED backing files. A recent
  2 MiB/group mapping window and cold-read residency eviction avoid retaining every
  historical index page in process RSS. Growth reserves actual blocks before the
  authoritative append, avoiding a disk-full sparse-mapping fault after logging.
  Truncation/snapshot cleanup discards unused mapping residency. These files are
  derived scratch and are reconstructed from the log after restart. Non-Linux
  keeps its heap-index fallback; no cross-platform resident-index claim.
- Expiry passes still attempt at most eight files/group and check 50 ms between
  operations. A confirmed successful deletion with more due work shortens the
  normal loop yield to 100 ms; no progress, unavailable quorum, or clock pause
  retains ordinary backoff when neither group can progress. A healthy group can
  still shorten the shared node loop while another is paused. Both groups and other GC/snapshot stages still run;
  an already-faster configured ordinary cadence is preserved. No sync is skipped.

The new retained-window test also exposed an existing W73-related certificate
bug: covered_index used snap_idx instead of log_base, overstating coverage when
snapshot-covered entries remain in the 512 MiB window. `e85b1b9c` corrects the base.
Tests explicitly prove a held unsynced append is not certified. No broad release
integration/other-platform qualification is implied by this branch fix.

## Native and memory qualification

Final normal native Raft store, Raft, KV LSM, expiry and metadata tests PASS.
Store regression covers 100,000 entries, eviction/checked reads, snapshot,
rotation, suffix replacement, cold reopen, corrupted-frame refusal and held-sync
watermarks. Linux RLIMIT_FSIZE fault proves index reservation fails before a
log entry is published. Actual GC idle policy tests and both management CLI
regressions PASS. AddressSanitizer/UBSan covers changed disk/log/index/CRC code
and the store regressions, with leak detection; PASS. Other linked library code
is the native library, not a claim of a fully sanitized daemon.

Matched 2.1 million-entry replay retains exactly the same entry count and durable
command bytes. Cached command bytes fall to 4,193,910. Massif peak useful heap:
391,124,982 → 20,979,692 bytes. Fixed native diagnostic VmRSS 15,892 KiB, RssAnon 7,192,
RssFile 8,700 KiB. Index capacity 150,994,944 bytes is file-backed capacity, not process
resident allocation. Linux file page cache remains kernel-managed; this is not
zero-memory or zero-I/O storage. Bulk index walks can temporarily fault more pages;
phase/steady measurements, not a universal peak bound, govern live acceptance.

The first native 565cc5cf store run FAILed new watermark assertions, prompting
the base correction before live deployment. The native-march baseline Massif
probe hit unsupported Valgrind instructions (SIGILL in crc_init); a generic-x86
diagnostic build completed. These are retained and do not count as qualified
passes. Generic diagnostics are distinct from the native live build.

## Live backlog comparison

Same committed workload and host as the historical run; sequential measurements,
not simultaneous controlled A/B trials. Each phase contains 10,000 fsynced 1 KiB
files. Times run from drain monitoring start; the restart/off-on phases include
pre-monitoring setup and their deadline-to-empty values are recorded separately.
RSS is the highest sampled individual daemon, sampled about every two seconds.

| Phase | Old namespace / physical seconds | Fixed namespace / physical seconds | Old / fixed peak RSS KiB |
|---|---:|---:|---:|
| staggered | 1397.978 / 1398.617 | 215.625 / 216.298 | 189376 / 61456 |
| mid-drain restart | 1329.624 / 1330.297 | 211.802 / 212.494 | 269024 / 73892 |
| disable/re-enable | 1334.826 / 1335.466 | 218.001 / 218.831 | 401620 / 72568 |

Fixed creation times: 72.264 / 72.343 / 73.509 seconds versus original
66.911 / 62.325 / 64.105. Creation itself did not get faster. Fixed staggered
foreground baseline/cleanup median 18.950/19.602 ms, p95 21.750/29.156 ms,
max 22.499/59.141 ms; 1,807 cleanup samples. Restart baseline/cleanup median
19.765/20.568 ms, p95 21.761/32.892 ms, max 28.018/182.228 ms; 1,752 samples.
Off/on baseline/cleanup median 20.521/20.839 ms, p95 22.112/33.019 ms,
max 26.030/277.594 ms; 1,800 samples. Original cleanup p95 was 26.124 / 25.859 / 26.402 ms. The shorter expiry yield
improves backlog latency while increasing foreground tail latency, especially
the restart phase; it is not an improvement in every latency metric.

Restart retained 9,992 pending files. The off phase preserved all 10,000 overdue
files. All phases retain renewed controls and independently verify physical fragments
and cold namespace state. No expiry errors; staggered/restart GC error counters
zero. Off/on n1 sampled reap_errors=3. Its apply log reports EFS_ERR_BUSY (-13)
for reap commands; batch apply can defer while leases or append rows remain.
This counter is not zero and the exact individual deferral condition was not
instrumented. Final queue, physical inventory, cold quota and late-PUT gates
passed; these retries did not strand the workload. Separate broad GC counter
classification is not changed here.

All 30,000 files PASS, about 6.1–6.5 times shorter measured drain. Maximum daemon
RSS across phases was 72.2 MiB versus 392.2 MiB previously. The final lower peak
than the restart phase is evidence against monotonic growth for this run, not
an unlimited-duration memory guarantee. Sampling counts 107/105/108; minimum
free physical space 210,976,276,480 bytes. The shortened drain necessarily adds
fewer foreground churn operations, so matched-history profiling supplies the
independent causal memory comparison. Normal expiry 24/24, core POSIX 216/216
and peer 67/67 PASS, with deletion queues, physical counters, restart inventory,
zero quota and late PUT fencing PASS. Clock-fault lifecycle 25/25, core 216/216 and peer 67/67 PASS.

## Reproduction and evidence

Same NUC Intel i5-1135G7, four cores/eight threads, Linux
6.12.0-211.64.1.el10_2.x86_64; /data1=/dev/sda XFS. Four fresh private 1 GiB-quota
nodes, TCP/direct-I/O/FILES, EFS_EXPIRY=1 except deliberate off phase;
EFS_COMMANDS=0/EFS_COMMANDS_SERVER=0.
Primary mount for backlog; peer mount for regression. Existing stores untouched.

```sh
make -j4 EFS_FAULTS=0 EFS_GIT_ID=afbce294 \
  EFS_GIT_DESCRIBE=afbce294 EFS_GIT_BRANCH=devel-expiry \
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
  --root /data1/efs/evidence/expiry-afbce294-backlog \
  --peer-mount /data1/efs/mnt2 --port 24410 --jobs 4 --backlog-files 10000
```

Native build O3/debug/frame pointers/march/mtune=native. Identical committed live
workload to [baseline](expiry-backlog-20261012.md), including 1 KiB fsynced payloads,
eight creators, 180 s lead, staggered bins, mid-drain restart, disabled-overdue
preservation, renewed control, physical/cold checks and foreground cycle samples.
Normal live run PASS. Clock-fault lifecycle 25/25, core 216/216 and peer 67/67 PASS.

Native build /home/efs/devel-expiry-afbce294-normal; build/native/run logs
/home/efs/expiry-afbce294-{build,native,backlog}.log. Primary evidence
/data1/efs/evidence/expiry-afbce294-backlog/gc-direct-uyhfs826. Heap/sanitizer captures and
private copies in /home/efs/expiry-rss-profile-20261013. Raw artifacts stay out
of Git. Never point the probe at user storage: opening a log replays/truncates
torn/reserved tails. Copy a stopped fixture first. Reproducer source:
tests/diagnostics/raft_heap_probe.c (removed from the tree on October 21 as an unrun orphan; read it with `git show a479018f6:tests/diagnostics/raft_heap_probe.c`). Build it with include/src/common/deps/blake3
headers and libefs.a, pthread/lm/dl/libibverbs; use generic-x86 objects for
raft_disk.c,raft_log.c,checksum.c,common.c,batch_obs.c when profiling with Massif.

Live executable SHA256:

| Binary | SHA256 |
|---|---|
| efsd |fba18a306faff9f1aecb6359784b922bfba2beacd7285dec953cb96b1e20166f|
| efs-fuse |f0806a4e5bce7d7fa267185887ceb2251cc8742065b4e4b4ee7777633d723c02|
| efs-mgmt |0cfc35cdac8c4032128fa30c8216a36b16beb9eb2d96fd55b71cbd1546a3f576|

Diagnostic reproduction (use only a copied, stopped test log):

```sh
cc -O2 -g -march=x86-64 -Iinclude -Isrc/common -Ideps/blake3 -I. -D_GNU_SOURCE \
  tests/diagnostics/raft_heap_probe.c src/raft/raft_disk.c src/raft/raft_log.c \
  src/common/checksum.c src/common/common.c src/common/batch_obs.c libefs.a \
  -lpthread -lm -ldl -libverbs -o /tmp/raft-heap-probe
/tmp/raft-heap-probe /path/to/private/copied/log-directory
valgrind --tool=massif --time-unit=B --max-snapshots=30 \
  --massif-out-file=/tmp/raft.massif /tmp/raft-heap-probe /path/to/private/copied/log-directory
cc -O1 -g -march=x86-64 -fsanitize=address,undefined -fno-omit-frame-pointer \
  -D_GNU_SOURCE -Iinclude -Isrc/common -Ideps/blake3 -I. tests/test_raft_store.c \
  src/raft/raft_disk.c src/raft/raft_log.c src/common/checksum.c \
  src/common/common.c src/common/batch_obs.c libefs.a \
  -lpthread -lm -ldl -libverbs -o /tmp/raft-store-sanitized
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 /tmp/raft-store-sanitized
```

The matched-log diagnostic additionally uses a historical captured fixture; it
is supporting causal evidence. The committed 100,000-entry store regression
provides deterministic reproduction without that private capture. Test store
regressions remove their own temporary stores. Live harnesses stop their owned
mounts/daemons and retain evidence directories; do not delete user stores.

Normal retained artifact SHA256:

| Artifact | SHA256 |
|---|---|
| source archive | 3c4a67959ee0c6843cd09640ae62dc721f4701294680124ff3a19640726bf8dc |
| run log | ea2666d35db0f1de1afa6d2e7688d11f0184e90f4f819293b704366dee844736 |
| manifest.json | 801e9705482e7fd309a01ea00d4b170c5958f3b87b726d2d18c83498b045c039 |
| expiry-runtime.json | a8cc45f3190a5f489028e9dacc028850f326bff20b55d72ab23f6e94b9e13092 |
| expiry-lifecycle.json | ced811af7cb4910efab2ee6b36d8f5fe2a9db917ff103b86ed64ded754a6ef34 |
| expiry-backlog.json | 195d5043ccdbd8d9cd3264868161b1f2da495bad6a319dc8ea0d36a3cdc92518 |
| posix.tsv | 218ec4dd471a5c48d1c9e71ac9222692ae8c385f2c2629802282379080bb0206 |
| posix2.tsv | 4eea1508e64929579d17b19f5cb799aa021d96e2b53566ef5d77403dc3b94fbc |

Clock-fault reproduction: extract the same source archive into a separate clean
build directory, then run (the marker does not change the host clock):

```sh
make -j4 EFS_FAULTS=1 EFS_GIT_ID=afbce294 \
  EFS_GIT_DESCRIBE=afbce294 EFS_GIT_BRANCH=devel-expiry efsd efs-fuse efs-mgmt
bash tests/run_tests.sh posix expiry /data1/efs/mnt \
  --root /data1/efs/evidence/expiry-afbce294-fault \
  --peer-mount /data1/efs/mnt2 --port 24410 --jobs 4 --clock-fault
```

Build `/home/efs/devel-expiry-afbce294-fault`; build/run logs
`/home/efs/expiry-afbce294-fault-build.log` and
`/home/efs/expiry-afbce294-fault.log`; primary evidence
`/data1/efs/evidence/expiry-afbce294-fault/gc-direct-taw4w_rg`. Same host, four-node quota,
TCP/direct/FILES, disabled `.efs` configuration. Clock fault lifecycle
25/25 PASS, including pause, operator reset and resumed physical reclamation.
Aggregate PASS: core 216/216 (23.4s), peer 67/67 (11.5s), independent GC,
cold inventory, drained queues, zero quota and late-PUT fencing. Normal core
216/216 took 26.2s; peer 67/67 took 14.7s. Both harness commands exited zero.
Final cleanup verified both mounts unmounted and no efsd processes remaining;
private evidence stores retained and existing user stores preserved.

| Supporting artifact | SHA256 |
|---|---|
| fault efsd | 8a786673f60eed97f66e3c28b309d23b6cdf66dac91ccad10fc0c9cefba29ab7 |
| fault efs-fuse | e3345f19d632ea24b6beb1b5a764b230ec305972b8a87fea525b84dad77bad82 |
| fault efs-mgmt | 7708da449503cb80e9e7f27a1050952b86870920d6cc5ad6a109b5475a8c829f |
| baseline-generic.massif | c90f2bcb80d5bc9ea5b41dc9642a244dc0088a8c372451082be65f35f50c5653 |
| fixed.massif | 5827ac05015c2f5b368f8ccd7ea979f888b617f9ffdd22308b6b0041e81b073a |
| sanitizer.log | 63d179d935ca71441460b6158395b52b1b6d07454a84601644fbfef60328909e |

| Fault evidence | SHA256 |
|---|---|
| run log | 61d10fff4ced55b3aa2eeb15598055420922bb919accb3de3a3c0e50446f9ebf |
| manifest.json | 5ff8e8b8a84d8ad3071e81424e1d602f09f747ddd126f66b0c2a70e5dda2e186 |
| expiry-runtime.json | 611936e67054fb160e8b0fed9fde04216592599d1a1024a5e9ef8170c9d69dd9 |
| expiry-lifecycle.json | 7b54b1de4128bd6a603b214a6f58e394ec22b7666c0cda7e332c1568b697ff5a |
| posix.tsv | fcc06493a28a42380e8f1ecfe55f326bb3b54b0f3f91229e2dd382e3f75ab7f2 |
| posix2.tsv | c694f6024d69c70c75ae76fbb655f25a0d72c1d76f49a4cc717af02bf21fd1d6 |

## Foreground latency follow-up

Historical results above are preserved. The subsequent
[foreground fairness investigation](expiry-foreground-latency-20261013.md)
measures smaller expiry bursts plus separate broad housekeeping cadence while
keeping physical GC fast: p95 improves 20–40% and physical drain improves
25–29% in the matched 30,000-file run. It records the rejected experiment,
remaining maximum-latency outliers and exact source/configuration.
