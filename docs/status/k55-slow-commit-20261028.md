# K55 — slow storage COMMIT, independent fcstor015 qualification

**RUNNING; not yet a passing live qualification.** The user assigned K55 to
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
K55. The first TCP slow-COMMIT case passes; subsequent live checks are running.

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
Final gate results, stable capture manifest and cleanup receipts are pending.
