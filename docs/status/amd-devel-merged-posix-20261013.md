# Merged devel: fresh AMD ordinary-file POSIX qualification

Source: `96f40602b7bfc87d2837b1bd9e5cdaecc97c203c`. All eight local branch
tips present before integration are ancestors of this commit. Documentation
conflicts retain AMD evidence and the newer ORCD acceptance checkpoint; no code
conflicts occurred. This does not close outstanding package acceptance gates.

`devel-amd-package2` was deleted after integration and its worktree detached.
No `devel-amd-package1` branch existed. `devel-orcd-package1` is a different
branch and was retained. No push, tag or release promotion.

## Deployment and reproduction

AMD-395, Ubuntu, four same-host daemons at 127.0.0.1:17432–17435,
17G logical quota each, two FUSE mounts `/data1/efs/mnt` and `/data1/efs/mnt2`.
Files storage, TCP, direct storage I/O. All six process environments verified:
`EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_EXPIRY=0
EFS_REPAIR_EXPERIMENT=0 EFS_TRANSPORT=tcp`. Existing GC admission=1 and idle=20ms retained.

Export `git archive 96f40602b7bfc87d2837b1bd9e5cdaecc97c203c` to
`/data1/efs/devel-merged-src` on AMD. Compile there:

```sh
make -j8 EFS_GIT_ID=96f40602b7bf EFS_GIT_DESCRIBE=96f40602b7bf EFS_GIT_BRANCH=devel efsd efs-fuse efs-mgmt
```

Stop with `EFS_CLUSTER=amd /data1/efs/devops/stop.sh`, verify no efsd/efs-fuse
processes or EFS mounts, then erase only the four synthetic `/data1/efs/n1`–`n4`
stores and `/data1/efs/.mkfs-done`. Install the three binaries and current
`scripts/client.sh` under `/data1/efs/src`. Set AMD config command defaults to
zero. Start with the above environment plus `EFS_CLUSTER=amd EFS_DIRECT_IO=1`.
Initial mkfs returned rc=-15 before quorum election, so first mount failed;
explicit `efs-mgmt raft-mkfs 127.0.0.1:17432` after election returned rc=0,
then start succeeded. Verify empty mounted namespace and actual /proc process
arguments/environments before testing. Unrelated host services untouched.

Binaries built 2026-10-13T20:42Z; SHA256:

- efsd: `1045581831c8b426ba4ba2843f3ff7605ba6988ddf60286734807de8cdd76c7b`
- efs-fuse: `a70d8320ce1138520749ec8d7676cd408f0fb64461bb1e896c29ca22750df229`
- efs-mgmt: `35a03f2a7dc0b48dd0f5e4a335aec0679f4694184438b8a63642892f93bb4524`

Run the basic core suite, serially, with default original per-case deadlines
(default minimum 15 seconds; longer decorator budgets retained):

```sh
cd /data1/efs/devel-merged-src
python3 tests/posix/posix_suite.py /data1/efs/mnt --suite core --jobs 1 --results /data1/efs/devel-merged-posix.tsv > /data1/efs/devel-merged-posix.log 2>&1
```

Expected: zero FAIL/NOTRUN, successful exit. Runner cleans its owned test
fixtures; leave the cluster running. No extras/query,
expiry, dedicated recovery or loaded seven-client acceptance is implied.

## Verdict

PASS: core POSIX **216/216**, zero failures/skips/notrun, exit 0, **44.0 seconds**.
Build and deployment PASS. Raw build/test logs
remain outside Git at `/data1/efs/devel-merged-{build,posix}.log` and
`/data1/efs/devel-merged-posix.tsv`.
