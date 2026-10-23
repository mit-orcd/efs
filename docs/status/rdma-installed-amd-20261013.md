# Installed AMD segmented RDMA qualification

Historical0ec deployment baseline. Current fixes/deployment and affected
reruns are in the [load/fallback ledger](rdma-load-fallback-fixes-20261013.md).

W106 / [segmented transport contract](rdma-segmented-messages-plan-20261013.md).
Branch `devel-rdma`; installed runtime **0ec2a869344734176923c1219ac7cd8ef1bf038e**.
Production transport matches prior a32567d7; subsequent commits add test harnesses.
This supersedes the deployment state in the [isolated integration checkpoint](rdma-soft-roce-integration-20261013.md), without changing its historical verdicts.

**Partial qualification: serial/peer/cold/reconnect PASS; loaded deep-directory
and paused-follower read FAIL.** Snapshot pressure/cleanup failed its original budget; actual transfer remains NOTRUN.
No TCP control comparison is required or being run for these failures, per user
instruction. No push, merge into devel, tag or hardware-IB acceptance.

## Deployment and identity

AMD-395 / Ubuntu, same-host Soft-RoCE rxe0, port1 ACTIVE, GID1, backed by
wlp194s0. Four installed daemons 127.0.0.1:17432–17435, `/data1/efs/n1..n4`;
files storage, direct I/O, **17G quota per node**, retained namespace. Both FUSE
mounts `/data1/efs/mnt` and `mnt2` use strict `EFS_TRANSPORT=rdma`.
Client `EFS_COMMANDS=0`, server `EFS_COMMANDS_SERVER=0`, `EFS_EXPIRY=0`, `EFS_REPAIR_EXPERIMENT=0`; containers OFF. Defaults persisted
in `/data1/efs/devops/clusters/amd.env`. No timeout/window/snapshot-threshold
changes. This retained 17G fixture is not the historical fresh 4G load fixture.

The original tiny-files writer was gracefully terminated and drained before
unmounting. Installed stores and namespace were retained, including 10GiB
`001.dat` and `tiny-files1`. Startup initially refused unsafe filesystem reserve
headroom. Authorized cleanup removed only stopped synthetic fixture node stores
from old gc/pressure/lease runs, recovering roughly 40GiB; original stores and
external evidence were retained. Safety checks were not bypassed.

Build from exported runtime source, installed complete source at `/data1/efs/src`
with `SOURCE_COMMIT`; private build `/data1/efs/evidence/rdma-deploy-src`:

```sh
make -B -j8 EFS_GIT_ID=0ec2a8693447 EFS_GIT_DESCRIBE=0ec2a8693447 EFS_GIT_BRANCH=devel-rdma efsd efs-fuse efs-mgmt tests/test_rdma_segmented tests/test_rdma_xprt
```

Installed binary SHA256:

- efsd: `ec42f3dacc5f101e79da5392bacd82b98661aa08c28c1f876a2e8090489f9ad9`
- efs-fuse: `aaf26851fbbd4e00f3d90bfd4aa3f15892defbf807e9fbed298b2fad9dd54af4`
- efs-mgmt: `9d544f53d960f0013794f2c3a63c60634be621842edf9f7bc510d35f0ad908bf`

## Gate ledger

| Gate | Verdict / observation |
| --- | --- |
| Codec, wrapper budget/slot ownership, setup-reentry regression | PASS |
| Normal, interrupted assembly, illegal post-setup TCP byte, original deadline | PASS; illegal-byte fault injection is not a TCP comparison |
| Same-process reconnect/generation churn | PASS three native repetitions, 48 connections total; zero live connections/global assembly after each cycle |
| Full-common-library ASan/UBSan reconnect churn | PASS; unchanged a325 transport library plus final 0ec test source, leak detection enabled |
| Installed serial core POSIX | PASS 216/216, 47.9s, no skips/notrun |
| Installed two-client peer POSIX | PASS 67/67, 8.3s |
| Seven clients, jobs16 each, distinct tags | FAIL 1510/1512; two `dir_deep_nesting` 15s timeouts, clients2/3 |
| Affected deep cases alone | PASS 2/2, 0.8s; does not erase loaded failures |
| Prepare, clean unmount/all-daemon restart, cold verify | PASS 27/27 prepare, 27/27 verify; stock 60s case budgets |
| Existing 10GiB file with follower SIGSTOP | FAIL read availability: EIO at offset4,049,813,504; decode rc=-9 after repeated RDMA setup failures to paused node. No corruption established |
| Existing 10GiB full digest after follower restoration | PASS exact original SHA256, all10GiB read; cached read timing is not a performance/cold-disk claim |
| Natural snapshot pressure / cleanup | FAIL common630s finish budget; 30/32 writers completed with no oracle errors, two group0 writers did not finish |
| Actual snapshot transfer / fixed target catch-up | NOTRUN: local export observed, no import; exception prevented fixed-watermark gate |
| Restored follower normal applied progress | Observed both groups applied1139392 /178228 after restoration; no frozen-target acceptance claim |
| Post-fault quiet core / peer | PASS216/216 (46.1s),67/67 (11.8s), unchanged budgets |
| Hardware cross-host IB and broader required fault matrix | NOTRUN |

Per-client core passes/durations: c0 216/76.1s; c1 216/77.0s; c2 215/50.5s;
c3 215/76.0s; c4 216/77.5s; c5 216/77.4s; c6 216/77.6s.
The loaded gate used stock case budgets and outer180s, not larger deadlines.

Reconnect regression `79388de7` initially treated a peer FIN after complete
verified duplex messages as unexpected payload. `0ec2a869` corrects that oracle:
zero-length EOF is allowed; an actual application TCP byte still fails. An
incremental build then reused an older test executable because source timestamps
were older; force-rebuilt test binary and three repeats passed. Failed artifacts
remain external. No production defect was fixed by this test-oracle correction.

Snapshot helper provenance: `6431a5ee`, `aa957f09`, tested **8883952b**.
After the failure, **a8a2e2e7** changes cleanup only: terminate every owned worker
before waiting, share the existing30s/5s cleanup budgets, record child PIDs, and
retain the original exception. Syntax checked; this new cleanup path has not
been live-fault qualified. Runtime
binaries remain0ec. One-writer-per-group attempt was intentionally interrupted
because it could not reach the natural retained-log threshold within600s; it
restored its paused follower. The parallel attempt uses16 independent inode/
xattr writers per group, each existing600s workload; shared setup30s, common
630s finish budget and original180s applied catch-up. No snapshot threshold or
operation timeout is changed. Require successful actual import on both groups,
not merely a helper exit or local snapshot creation.

Observed group0 export: index1094450,140,767,188bytes (~134MiB),rc0.
Completed writers: group0 fourteen,110,944updates; group2 sixteen,86,977updates;
all completed oracles PASS. Two group0 processes stalled before completion.
The original630s limit raised TimeoutExpired; cleanup initially raised a second
exception waiting for another blocked worker, obscuring the first. Follower
SIGCONT had already run, and explicit process inspection verified all pressure
workers eventually gone. Do not call this a passing snapshot or catch-up gate.

Closed seven-load-client socket audit:910/910 measured,post_setup_tcp0/0,
violations0. Installed daemon/two-client audit sampled1423 closed connections,
195 with multi-segment sends, but ten paused-node teardown counters reported
0/1 with violations0. The accounting implementation subtracts received FIN only
in TCP_CLOSE_WAIT; no state capture proves these ten bytes are FINs, so full
zero-application-byte proof remains unresolved. These are accounting diagnostics,
not a TCP transport control run.

## Reproduce and retain evidence outside Git

Prerequisites: exclusive installed AMD fixture, strict RDMA settings above,
sufficient filesystem reserve, all four nodes up. Drain external writers first.
Use fresh external result directory `$out`, not under a FUSE mount.

```sh
cd /data1/efs/src
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=rxe0 EFS_RDMA_GID_INDEX=1
export EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_EXPIRY=0 EFS_REPAIR_EXPERIMENT=0
make test-rdma-message
python3 tests/test_rdma_reply_budget.py
python3 tests/test_rdma_setup_reentry.py
for mode in normal interrupt tcp-violation deadline churn; do timeout 45s tests/test_rdma_segmented "$mode"; done
python3 tests/posix/posix_suite.py /data1/efs/mnt --suite core --jobs 1 --results "$out/core.tsv"
python3 tests/posix/posix_2client.py --local /data1/efs/mnt /data1/efs/mnt2 --parent rdma-installed-peer --results "$out/peer.tsv"
python3 tests/posix/posix_suite.py /data1/efs/mnt --filter dir_deep_nesting --jobs 1 --tag rdma-deep-isolated --results "$out/deep.tsv"
```

Seven-client reproduction: create seven distinct mounts under AppArmor-permitted
`/home/efs/rdma-installed-load-0ec2a869/c0..c6`, each using
`scripts/client.sh 127.0.0.1:17432 MOUNT`. Concurrently launch for i=0..6:

```sh
timeout -k 5 180s python3 tests/posix/posix_suite.py "/home/efs/rdma-installed-load-0ec2a869/c$i" --suite core --jobs 16 --tag "rdma-seven-$i" --results "$out/seven-$i.tsv"
```

Wait every process, preserve each exit code. Cleanly drain/unmount all owned
clients using `scripts/client.sh stop MOUNT`; do not leave processes running.
Never infer PASS from just a running mount or a missing result file.

Cold persistence: run `tests/posix/posix_persist.py /data1/efs/mnt --phase prepare
--tag rdma-installed --results "$out/persist-prepare.tsv"`; cleanly stop both
installed mounts, then `EFS_CLUSTER=amd /data1/efs/devops/stop.sh` and `start.sh`.
Verify same command with `--phase verify` and a new result file. Uses existing
stores and removes only its owned fixture after verification.

Actual snapshot gate, after copying final committed helper to installed source:

```sh
python3 -u tests/live/rdma_snapshot_lag.py /data1/efs/src /data1/efs/mnt "$out/snapshot-parallel" --writers-per-group 16
```

Harness validates shared FOLLOWER role before SIGSTOP and restores SIGCONT in
`finally`, including termination. Inspect selected follower's `raft-snap: import
end group=0/2 ... rc=0` and leader export byte counts; require multi-segment
transfers, every writer's attribute/payload oracle, and both fixed applied
watermarks. Do not start competing diagnostics during the measurement.

External raw evidence: `/data1/efs/evidence/rdma-deployed-0ec2a869`; installed logs
`/data1/efs/logs/efsd-n*.log`, `/data1/efs/efs-fuse-mnt*.log`.
Core TSV SHA256 `9c9bb4f86b439aad72da09ff77fc7652c12cadad6a10d6dadbd0f460197b18fe`;
peer `b48395b65f5afb845b90c2e0798f82273d964ac08e6ef4e3c8fff166e46f44b4`;
cold verify `e39ec67ce85aee4844e03f9749eaa4ad206dff94a37ab63e29cf067fa97fb9f1`;
post-fault core `bce6e4b534d795c666a23c69861f0aa9ea10e9d5b6ff1de7ca4d4c07dda347f2`;
post-fault peer `818552927f044a0d8b33c812578a44ef7dadfbf8822dbdd022f202cde62d970e`.
Predeployment10GiB digest:
`7aa0c22312679872fd8771e222398ee44be40c60ece4ded8e76896c9ef02625b`.
Do not label the interrupted digest read a mismatch: it did not finish reading.
The restored complete read used8MiB Python reads and SHA256 over every byte;
matching digest and exact10GiB length are required. Reproduce independently:

```sh
python3 - <<'PY'
import hashlib
h=hashlib.sha256(); n=0
with open("/data1/efs/mnt/001.dat", "rb", buffering=0) as f:
    while True:
        b=f.read(8 << 20)
        if not b: break
        h.update(b); n+=len(b)
assert n==10737418240
assert h.hexdigest()=="7aa0c22312679872fd8771e222398ee44be40c60ece4ded8e76896c9ef02625b"
print("PASS", n, h.hexdigest())
PY
```

This checks the retained original fixture, not arbitrary new user data. For a
new fixture record its digest before fault injection. No throughput claim from
this warm post-restoration read.

Remaining: complete snapshot proof, teardown-counter attribution, focused
RDMA diagnosis of loaded metadata deadlines and degraded fragment read admission/
fallback. Retain current failures and original budgets. Software same-host
qualification cannot close hardware IB or production rollout acceptance.

Final checkpoint: all four installed daemons and both clients running strict
RDMA, requested features OFF, no paused daemons/pressure workers/screens.
Removed only four exactly identified owned snapshot-pressure payload directories
left by interrupted/timed-out tests. Original10GiB file and tiny-files namespace
retained. The previously stopped user writer was not automatically resumed.
For user-directed resumption, from `/home/efs` use the original command with
`--resume`: `python3 ./tiny_files.py --resume --max-depth 8
--max-files-per-folder 1000 --max-files 1000000 --workers 4
/data1/efs/mnt/tiny-files1`.
