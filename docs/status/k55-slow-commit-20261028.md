# K55 — slow storage COMMIT, independent fcstor015 qualification

**FIXED on `agent/devel-k55`; independent focused qualification PASS.**
The full gate executed all 546 commands with **0 FAIL / 4 NOTRUN**: its
blanket verdict remains INCOMPLETE, not a release PASS. The user assigned K55 to
its own branch, `agent/devel-k55`, on fcstor015, with every failed fixture
retained there and other agents' clusters excluded. No merge or push.

## Failure and correction

The [Task AI evidence](ai-rdma-registry-lock-20261028.md) preserves the original
RDMA full-gate failure and an independent untouched-base reproduction:
F0 fsync returned EIO at 60.01 / 60.04 seconds. The `PDD` snapshot contains
two durable fragments. The coordinator joined every storage-COMMIT worker
before folding those results, exhausting its 60-second sweep budget before
it could commit the required degraded mark. This is K55, distinct from K54's
member-down session-revoke failure. The original failures are not relabelled.

The product correction was independently committed by the K35 session as
`1ab9fef83`, reused with attribution by cherry-pick `478a5400f` here. Completed
worker results are published under a mutex and condition variable. The
coordinator can use two completed, matching durable proofs to mark degraded
protection while budget remains; unfinished workers keep their original
absolute RPC deadlines and are all joined before return. A failed mark is
not retried merely to obtain success; later independent three-fragment proof
can still succeed. Insufficient proof retains obligations and returns failure.
No format, product flag, durability requirement or deadline was changed.
The coordinator reserves at most five seconds (one quarter of a shorter
remaining budget) for metadata proof; this is an internal scheduling point,
not an extended timeout or a guarantee for arbitrarily large backlogs.

## Source, environment and reproduction

The immutable independent candidate is **`5185bfcd3`**, including the committed
controller [k55_qualify.py](../../tests/measure/k55_qualify.py). Branch base
`9c5634d22` is Task AI's closure: product inputs before this fix match devel
`486aa3a46`; its measurement helpers and explicit strict-transport test wiring
are retained. Other K35 changes were not imported.

fcstor015: AMD EPYC 7402P, 24 logical CPUs, two NUMA nodes; RHEL 9.8,
kernel `5.14.0-687.39.1.el9_8.x86_64`; NVMe `/data1/01`; active mlx5_0
Ethernet/RoCE, firmware `16.34.1002`, MTU 1024. Tests use mlx5_0, private
loopback logical clusters and both TCP and real RDMA. Fragment I/O is O_DIRECT.
This is not multi-host fabric, physical-node-loss or power-loss acceptance.
The B6/K35 sources retain their fixture sizes, quotas, packing/recovery and
failure rules; no configuration was reduced to obtain a pass.

Private libfuse is `~/orcd/scratch/efs/orcd-test/deps/libfuse` with its pkgconfig
and runtime library paths. GCC is 11.5.0-14.el9. ASan/UBSan are installed;
the matching libtsan RPM is privately extracted, not installed system-wide.
RPM SHA256 `4e407df94292ad96486d01fd1b0e272ecd599ed3dfbef959fe61a2eb4032f13a`.
The private compiler wrapper only supplies the matching TSan library prefix.
No system runtime, ASLR setting or other user's configuration was changed.

Export the source into a **new** private run directory, complete extraction,
then write its `EXPORT_COMPLETE` marker. The tested invocation is:

```sh
python3 -u /data1/01/efs-k55-20261028/src/tests/measure/k55_qualify.py \
  /data1/01/efs-k55-20261028 /data1/01/efs-k55-20261028/src \
  /data1/01/efs-k55-20261028/qualification-5185bfcd3 \
  /data1/01/efs-k55-20261028/old-data_commit.c 5185bfcd3
```

The old C input is exported from `9c5634d22:src/client/data_commit.c` solely
for a synthetic negative control, not for old-store replay or compatibility.
Set `EFS_FUSE_PREFIX`, its `PKG_CONFIG_PATH` and `LD_LIBRARY_PATH` before
launching. The controller records binary hashes and exact command environments,
uses fresh per-stage temporary directories, checks all-user process metadata
for competing work, and stops on failure. It never stops foreign work.

## Gates and retained evidence

Native component and 65,536-chunk index tests, ASan/UBSan with leak checking,
and TSan **PASS**. The old production implementation aborts on the new
slow-proof assertion as expected, demonstrating that the regression detects
K55. All four planned slow-COMMIT cases pass; the completed full gate also passes
the formerly failing B6 slow command.

| Transport / repetition | F0 fsync | Durable degraded marks | Exact acknowledged files after restore/restart | Case duration |
| --- | --- | --- | --- | --- |
| TCP 1 | OK, 60.04 s | 3 | 22/22 | 205.2 s |
| RDMA 1 | OK, 60.03 s | 3 | 22/22 | 221.2 s |
| TCP 2 | OK, 60.03 s | 3 | 22/22 | 208.5 s |
| RDMA 2 | OK, 60.00 s | 3 | 22/22 | 209.1 s |

All seven fsyncs in each case succeeded; the other six took 8.00–8.05 s.
No file was refused and every case reports an empty cleanup-error list.
The approximately 60-second F0 is expected: its worker is still joined, even
after durable degraded marking has succeeded. This fixes availability, not
the time spent waiting for that slow worker. Product `data_commit.c` SHA256:
`f3c525150a87d5478144362b6953c27e98360e2a92bb555b9bd1ce085deefe56`.

The fixed plan is two repetitions of B6 slow mode **per transport**, then K35
degraded-fsync checks on both transports, RDMA transport/segmentation modes,
RDMA reclamation plus all 218 core POSIX checks, and the full `test-all` gate.
Slow cases retain the original 65,000 ms injected COMMIT delay, 60-second
product sweep budget and 75-second syscall bound. Every planned repetition
must pass; no repeat-until-green. The controller additionally requires all
seven fsync calls to succeed, at least one degraded mark, all **22**
acknowledged files exact after restore and restart, and no refused file.
Required component/live stages may not skip. Full-suite NOTRUNs are disclosed
separately and never counted as executed passes.

Raw logs, state, JSON checks and fixture roots stay on fcstor015:

* `/data1/01/efs-k55-20261028/qualification-5185bfcd3/`
* `/data1/01/efs-k55-20261028/tmp/`
* `/home/erbmi1/efs-artifacts/k55-20261028/fcstor015/controller-5185bfcd3.log`

The original Task AI failures and sealed evidence remain untouched at the
locations/hashes on its evidence page. New failed evidence is retained too.
Checked retirement of owned processes, mounts and ports is required before
cleanup; a legacy K35 cleanup after an intentionally dead member does not
qualify K54's clean session revocation. K53 is likewise not closed by this fix.
K35 durability assertions PASS on TCP (148.1 s, 172 exact files) and RDMA
(155.6 s, 196 exact files), including 3/4/5-member placement, EIO with two
members down, repair and death during COMMIT. Both final member-down unmounts
actually log `flush rc=0 settle rc=0 revoke rc=-13`: **K54 remains a failed
clean-unmount observation**, despite the harness durability PASS. Its two
fixture stores and logs remain kept for that open investigation.
RDMA xprt and all seven segmentation modes PASS (normal, interrupt, forbidden
TCP application traffic, deadline, burst, asymmetric burst and churn).
RDMA reclamation PASS, core POSIX **218/218**, two-client POSIX **68/68**,
including physical deletion, cold restart, offline-member reap-ledger recovery
and late-PUT fencing.

## Full gate and closure

The controller ran the following from that immutable source, with the build
prerequisites above:

```sh
EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 \
EFS_TRANSPORT=rdma EFS_B6_KEEP=1 EFS_K35_KEEP=1 \
TMPDIR=/data1/01/efs-k55-20261028/full-tmp bash scripts/make_test_summary.sh
```

Result: **546 commands, 0 failures, 4 NOTRUN,
671 seconds testing plus 16 seconds build** (687.65 seconds controller wall
time), exit 3 / INCOMPLETE. The seven advisory overbudget observations remain
in the retained summary (K35, K43, Raft ACK window, K22 admission and three
D39 model shards); no budgets or test deadlines were relaxed.

The four incomplete items are:

* sparse metadata: the host did not materialize the THP negative; its fixed
  small-page proof did execute;
* external cluster stop/remount: `~/git/cluster` scripts are absent;
* RDMA Memcheck: `valgrind/memcheck.h` is absent;
* sanitizer attachment probe: the host refused attachment. The separately
  compiled native component ASan/UBSan/leak and TSan runs above did execute
  and pass; they do not replace this skipped probe.

There were no new unexpected failed qualification commands and no unchanged
retry-to-green. The intentional old-source negative control aborted as required.
The existing K54 failure was observed in both targeted K35 runs and again in
full-suite case 465. It remains an alpha blocker. K53's original quota failure
is not waived by this full run's successful quota command. Distinct-host,
physical-node/power-loss and other alpha release gates remain outside this
single-host qualification.

Closed captures are copied, rehashed and fsynced on the same host by
[k55_seal.py](../../tests/measure/k55_seal.py), tools commit `9ec6fecf4`:

```sh
python3 /data1/01/efs-k55-20261028/tools-9ec6fecf4/k55_seal.py \
  /data1/01/efs-k55-20261028 \
  /data1/01/efs-k55-20261028/qualification-5185bfcd3 \
  /home/erbmi1/efs-artifacts/k55-20261028/fcstor015/sealed-final-5185bfcd3
```

The capture contains **165 verified text files**. Manifest SHA256:
`04c9258fc475176d3d3a66b401e3394e14bf1b2ff68839ce0290db988463d42b`;
closure SHA256:
`92445706f4e8bd991d2a41c5d825524110536c294722be12509be8c14083d344`.
It records no observed EFS/test processes and no owned mounts after completion;
controller exit is 0. All four recorded binary hashes and the product C hash
still matched after the full gate. No process was signalled or mount detached by this
closure tool, and no original was removed. `/home/erbmi1` resolves to its
normal NFS home; both sides of the sealer's destination comparison resolve
that alias. The first sealer invocation refused that lexical mismatch before
creating any output; the corrected tool retained the ownership/path checks.

All **three K54 fixture roots**, including full-suite case 465, are fully
inventoried with per-file hashes and metadata in sibling
`retained-k54-fixtures-5185bfcd3.json`, SHA256
`91467141f480ee2f3912e4ba3a59c3cbaa6378e7e8d38c22982b9549082120cb`.
They contain respectively 2,972 / 3,408 / 2,668 inventory entries. The original
Task AI sealed manifest still hashes to
`f65ef820843becf200ab35f647af919bfe26c48c1cc626b54fdb24f354257a6c`;
its failed fixtures were never deletion targets or opened by these clusters.

Candidate bulk is explicitly **kept for the open branch review and K54 cleanup
investigation**; no fixture, failed evidence or build was pruned. All raw data
stays on fcstor015. The product fix is ready for integration review. No merge or push; other
agents' clusters remain untouched.
