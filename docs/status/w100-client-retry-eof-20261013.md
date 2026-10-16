# W100 client retry EOF: Intel qualification

**Verdict: PASS for the targeted retry contract. W100 aggregate remains OPEN.**
Code `842a4c3d9` on devel replaces the repair's adopt-on-miss
`efs_client_stat_ino` with read-only `efs_client_rpc_getattr`. An undersized
remote EOF is no longer staged before validation, so a second snapshot cannot
mistake it for trustworthy local EOF. Missing trustworthy EOF still causes a
visible refusal; this does not recover an already lost original EOF.

Tested source was **b872bdc296de7eda561c9d2f7af18248543c2964**, which adds
only regenerated architecture documentation to the code fix. The initial full
native run on 842a4c3d9 stopped on stale generated architecture documents from
its prerequisite; b872bdc29 fixes that gate and both complete reruns passed.

## Environment and identity

Intel1 10.0.0.207 and Intel2 10.0.0.209, Core Ultra 7 256V, 8 CPUs and about
15 GiB RAM each, Rocky Linux 10.2 / 6.12.0-211.64.1.el10_2.x86_64.
Four private daemons (two per host), ports 27432/27433, files storage,
**TCP/direct I/O**, 2 GiB quota per node on data1/data2. Both clients mount
against the opposite host. `.efs` client/server and expiry
are disabled: `EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0
EFS_EXPIRY=0`, daemon `--direct-io`.
Runtime process environments and absent `.efs` were verified on both hosts.

Private binaries under `/data1/efs/w100-private/retry-fix` were built with
`EFS_FAULTS=1`, identity `b872bdc29-fault`, branch devel, timestamps
2026-10-14T02:40:51Z (Intel1) and 02:40:52Z (Intel2).
SHA256, efsd / efs-fuse respectively:

- Intel1: `d0dd54b065209877ec7f34e0474c45e270c19a2ac61efe9223a6af9179f59cf0` /
  `a4ad05d8e59d8d461179cedba6fbb43b69db0145a81875605736d70dac0b997f`.
- Intel2: `29ad164f2bcf529aeaadea2f569d00606a63ecb87c738ee80ec683ab9bb2b203` /
  `76298fcfccf9660108f03d81514a769aa9402d5cb12547c75d34ee4d423c76d2`.

Both deployed source manifests match all 1,252 archived files. Source archive
SHA256: `c2dd89acea961fd4fd75e69cefc27cb91382a4f5ca764d3a4004c0c7d80222d3`.
Normal native builds used the same source under `retry-normal`, without faults.

## Reproduction and results

Prerequisites: existing Intel SSH aliases, build dependencies, mutual peer SSH
for the live oracle, and private ports allowed between hosts. Use the committed
`scripts/intel_w100_private.py` with staged source/binaries in `retry-fix`:

```sh
python3 scripts/intel_w100_private.py start retry-fixed retry-fix
```

Run on each host from `/data1/efs/w100-private/retry-normal`:

```sh
make -j8 -B EFS_GIT_ID=b872bdc29 EFS_GIT_DESCRIBE=b872bdc29 EFS_GIT_BRANCH=devel efsd efs-fuse efs-mgmt
EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_EXPIRY=0 make -j8 EFS_GIT_ID=b872bdc29 EFS_GIT_DESCRIBE=b872bdc29 EFS_GIT_BRANCH=devel test
CC='gcc -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 -no-pie' ASAN_OPTIONS=detect_leaks=1 python3 tests/test_report_retry_eof.py
```

**Full native PASS on both hosts; targeted ASan/UBSan PASS on both.**
The new regression extracts the actual production repair and next-snapshot
blocks with controlled RPC/staging adapters. Two attempts against remote EOF
0 or 1048577 remain refused without adoption; transient RPC BUSY remains safe;
restoring trustworthy local EOF 8388609 permits exact retry. A covering remote
EOF can be attached without local adoption. Negative control
`W100_SOURCE_REF=c3d8ee560 python3 tests/test_report_retry_eof.py` fails the
second-attempt assertion on old code. This is a controlled production-block
regression, not full-library sanitizer or mounted cache-eviction coverage.

On Intel1, then Intel2 (add `--peer intel1_efs` on Intel2):

```sh
python3 /data1/efs/w100-private/retry-fix/tests/live/report_eof_contract.py /data1/efs/w100-private/runtime-retry-fixed/mnt /data1/efs/w100-private/runtime-retry-fixed/omit-size --expect refused
```

**Live two-fsync refusal/recovery PASS in both directions.** For each 512-byte
and 8388609-byte sparse file, two consecutive faulted fsyncs return EIO while
the independent peer sees EOF 0. Clearing the omission fault allows the next
fsync to publish exact EOF. All 512 small-file bytes, sparse low/high anchors,
and EOF are checked: four cases, eight refused fsyncs, four successful retries.
The hook omits a size record after valid REPORT construction; it does not
physically evict a mounted staged row. Sparse anchors do not constitute a full
hole scan or complete-payload hash.

Run the following concurrently on both hosts, substituting each host in TAG:

```sh
timeout 900 python3 /data1/efs/w100-private/retry-fix/tests/posix/posix_suite.py /data1/efs/w100-private/runtime-retry-fixed/mnt --suite core --jobs 16 --timeout-s 30 --tag TAG --results /data1/efs/w100-private/retry-core.tsv
```

**Mounted core PASS: Intel1 216/216 in 62.6 s; Intel2 216/216 in 61.2 s.**
Zero failures, skips or notrun cases. Native bulk checks finished before these
concurrent mounted suites. No deadlines, receipt windows or retry budgets were
increased. The private startup preflight now uses SO_REUSEADDR, matching daemon
bind policy and avoiding false refusal on prior TIME_WAIT sockets; no listener
or host network setting was changed.

## Retention, cleanup and remaining gates

Raw logs, TSVs, source manifests, binary identities and runtime profiles are
outside Git at `/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/w100-retry-20261013/`.
Remote logs are `/data1/efs/w100-private/retry-{native-final,sanitizer-final,core}.log`,
`retry-core.tsv` and `retry-live-intel{1,2}.json` on the respective hosts.
The live files contain JSON plus a final PASS line.

Cleanup completed with
`python3 scripts/intel_w100_private.py stop retry-fixed retry-fix`: both clients
unmounted/drained and all four owned daemons stopped safely. Private storage and
logs remain for inspection. Production ports 17432/17433 and original main
Intel processes/mounts were left unchanged. No ORCD changes.

Peer/persistence suites and spontaneous staged-row eviction are **NOTRUN** in
this qualification. The historic rounding reproduction and server refusal
remain in [the prior report](w100-eof-contract-20261013.md); its c3d8ee560 client
repair is superseded here. This targeted PASS does not close other W100 loaded
EIO, concurrent timeout or lost-original-EOF acceptance gates.
