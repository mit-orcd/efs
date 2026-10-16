# Intel production and independent external clients — 2026-10-14

**PASS: Intel production upgraded; physical AMD and xorinox hosts mount its
export read/write at `/data1/efs/intel-mnt`.** Purpose: deployment readiness
for performance work under [ordinary-files package 3](three-work-packages-20261013.md).
No performance benchmark has been started by this task.

## Installed source and configuration

Frozen EFS source: `85f2a199aa391647a1ac49ab43f1a4f1e5d5880e`, branch `devel`,
wire build ID `85f2a199aa39`, version `v0.3.2-pre-alpha-450-g85f2a199a`.
Common build time `2026-10-14T18:23:50Z`. All 208 tracked Makefile/C/header/assembly
inputs match on all four hosts. Normal native GCC builds use the Makefile's
`-O3 -g -fno-omit-frame-pointer -march=native -mtune=native` settings; differing
host binary hashes do not imply differing source. The external launcher and
this documentation add no EFS C/data-path changes to that frozen release.

TCP, daemon direct I/O. `EFS_COMMANDS=0`,
`EFS_COMMANDS_SERVER=0`, `EFS_EXPIRY=0`.
Containers are unused. Persisted command policy remains version1/mask0;
`.efs` is absent through every mount checked.

|Service/client|Location|Configuration|
|---|---|---|
|Intel nodes1/2|10.0.0.207:17432/17433|`/data1/efs/n1`, `/data2/efs/n2`;400 GiB/node|
|Intel nodes3/4|10.0.0.209:17432/17433|`/data1/efs/n3`, `/data2/efs/n4`;400 GiB/node|
|Intel1/Intel2 clients|`/data1/efs/mnt`|Each seeds the opposite physical host|
|xorinox external client|10.0.0.201, `/data1/efs/intel-mnt`|Intel seed10.0.0.207:17432|
|AMD external client|10.0.0.205, `/data1/efs/intel-mnt`|Intel seed10.0.0.207:17432|

Intel/xorinox: Rocky10.2, Linux6.12.0-211.64.1.el10_2, libfuse3.16.2.
AMD: Ubuntu, Linux7.0.0-38-generic, libfuse3.18.2. `systemd-detect-virt`
reports no virtualization on either external host; no VM deployment was used.
All clients/daemons run as non-root user `efs` (uid1001).

|Host|efsd SHA256|Intel-client efs-fuse SHA256|
|---|---|---|
|intel1_efs|`e4e85723ca316918891bc7bbe5c92da6c761b9e96147e963c2087cb987dbddef`|`80bfe69e7886af4b163deb57e7ac34f8f8cb5662538f9de677b3c9d1ecaac067`|
|intel2_efs|`1633e531da4c757051f765385d14b4d05001394f37ae612b7eef750fa0240faf`|`ca01c73c1da391837e51a7917ef90b4c0b5ec738dad23b5cd4d5cad4e5d927c5`|
|amd_efs|Not installed|`f528fa8872721a1db8d4496033f8a1561d9a113996e2db24b36c59ba6a4eaa5b`|
|xorinox_efs|Not installed|`50f71470bb6818b03e260f0093410d3d2ebf62e26bd6fee263cd3e9d392f79b7`|

## Independent client deployment

On BOTH external hosts:

- Source/binaries: `/data1/efs/intel-client/releases/85f2a199aa39`.
- Dedicated source link: `/data1/efs/intel-client/src`.
- Standalone launcher: `/data1/efs/intel-client/client.sh`, copied from
  [intel_external_client.sh](../../scripts/intel_external_client.sh).
- Dedicated log: `/data1/efs/intel-client/logs/efs-fuse-intel-mnt.log`.
- Mount: `/data1/efs/intel-mnt`; source, transport and optional-feature flags
  are fixed by the standalone launcher, without sourcing cluster configuration.

Only `efs-fuse` and `efs-mgmt` are built there; no `efsd` binary or daemon is
installed in the client tree. `/proc` verifies each client's executable,
working directory and log all belong to this dedicated deployment. AMD's
existing cluster retains `/data1/efs/src`, its four daemons and two original
mounts. Their PIDs, arguments, executable hashes and profile remain unchanged.

Xorinox's initial client-only `/data1/efs/src` link created during this task was
removed. Its initial build/logs were retained inside `intel-client`; its VM and
cluster paths were not changed. The initial `/data1/efs/mnt` was cleanly drained,
remounted at `intel-mnt`, and the empty old mountpoint removed.

The existing AMD stop script globally scanned all EFS mounts. The mount-selection
correction in DevOps `e3d934b` now limits cleanup to configured cluster mounts.
The same correction was applied to AMD's older deployed stop script, preserving
its daemon shutdown implementation and a backup. Read-only selection returns
only `/data1/efs/mnt` and `/data1/efs/mnt2`, excluding `intel-mnt`. The older
AMD environment has `MNT`/`MNT2` rather than a `MOUNTS` array; the selector
handles both. Driver-side queries use only remote `findmnt`, so older peers
never receive a new flag that could enter shutdown. No existing AMD node was
stopped to test selection. Extra diagnostics now require their own lifecycle.

## Deployment and readiness evidence

Intel production previously ran `1a737143e543`; the latest fixes had only been
installed on the private cluster. Builds were staged before draining both
production clients while quorum was alive, then stopping the four production
daemons. Existing `/data1/efs/src` and `devops` directories were retained as
`*-before-85f2a199aa39-20261014` and replaced by release links. All four nodes
were launched before waiting for recovery; existing formatting markers caused
startup to preserve stores. No storage was erased or reformatted.

- Intel status: four up/zero down, all four versions identical.
- Original tiny-tree preservation:256 size/SHA256 samples per tree captured
  before upgrade and matched after restart through the respective Intel mounts.
- Both external mounts each verify all512 immutable samples from both trees,
  including after remounting from the independent client trees. Samples prove
  those bytes, not a new full two-million-file inventory.
- Fsynced xorinox write1,048,594bytes read exactly on both Intel hosts;
  Intel2's65,539-byte reply file read exactly on xorinox.
- A second smoke writes262,151/262,152/262,153bytes from AMD/xorinox/Intel2,
  respectively (`bytes(range(256))*1024`, followed by7/8/9 copies of byte0/1/2).
  Four parallel readers (AMD, xorinox, Intel1, Intel2) check exact length/SHA256
  of all three files. All PASS; only this owned smoke directory was removed.
- Independent launcher `stop`, `start`, `status`, shell syntax and final process
  profile checks PASS on both external hosts. DevOps read-only mount selection
  PASS on AMD and on the local AMD/Intel driver paths. The initial older-env
  empty-selection probe was corrected before any stop action was run.

The private Intel cluster on27432/27433 remains unchanged. AMD's original
local cluster remains running. Those services are part of the host background
state for future performance measurements. No test controller is left running.

## Host prerequisites applied

No build packages were missing. Xorinox/AMD initially received network rejects
from Intel's EFS ports. The human applied these source-restricted permanent
rules on BOTH Intel hosts, followed by reload:

```sh
firewall-cmd --permanent --add-rich-rule='rule family="ipv4" source address="10.0.0.201" port port="17432-17433" protocol="tcp" accept'
firewall-cmd --permanent --add-rich-rule='rule family="ipv4" source address="10.0.0.205" port port="17432-17433" protocol="tcp" accept'
firewall-cmd --reload
```

Connectivity to all four endpoints is verified from both external hosts. These
commands run on Intel; no Ubuntu firewalld installation/change was required.
The `efs` account cannot query the privileged rich-rule state, so persistence
is based on the human-confirmed permanent commands, not a claimed reboot test.

AMD's existing AppArmor `fusermount3` policy allowed only its old `/data1`
mountpoints. The first mount failed with a captured `failed mntpnt match` denial.
A candidate including these exact-path rules passed `apparmor_parser -Q -K`
validation before the human appended them to `/etc/apparmor.d/local/fusermount3`
and reloaded `/etc/apparmor.d/fusermount3` with `apparmor_parser -r`:

```text
mount fstype={fuse,fuse.efs-fuse} options=(nosuid,nodev) options in (ro,rw,noatime,dirsync,nodiratime,noexec,sync) -> /data1/efs/intel-mnt/,
umount /data1/efs/intel-mnt/,
```

Original rules were preserved. The subsequent AMD mount succeeds; no AppArmor
profile was disabled and no additional capability was granted.

## Reproduction and operations

The frozen build inputs can be reconstructed with:
`git archive 85f2a199aa391647a1ac49ab43f1a4f1e5d5880e Makefile src include deps scripts tests`.
Extract into a fresh versioned release directory on each target; never overwrite
an active cluster source tree. Build external clients with `make -j8
EFS_GIT_ID=85f2a199aa39 EFS_GIT_BRANCH=devel
EFS_GIT_DESCRIBE=v0.3.2-pre-alpha-450-g85f2a199a
EFS_BUILD_TIME=2026-10-14T18:23:50Z efs-fuse efs-mgmt`.
Intel additionally builds `efsd efs-query efs-bench`; all build flags/metadata
are retained in each release's `SOURCE.json`. Use the existing DevOps Intel
launch/wait/format-marker/mount phases, preserving all stores and the400G quotas.

External client management, as `efs`, on either host:

```sh
/data1/efs/intel-client/client.sh status
# Cleanly stop/restart only this Intel client when needed:
/data1/efs/intel-client/client.sh stop
/data1/efs/intel-client/client.sh start
```

`status` must show the exact `intel-mnt` FUSE mount and Intel's four-up matching
version result. Independently check
`/data1/efs/intel-client/src/efs-mgmt commands-policy 10.0.0.207:17432 get`
for mask0 and that `intel-mnt/.efs` is absent. The readiness smoke uses unique
owned directories, exclusive creates, fsync before peer reads, exact byte/EOF
checks, and successful-run cleanup; preserve any failing fixture for inspection.

Raw builds, process inventories, manifests and readiness results remain outside
Git under `/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-xorinox-deploy-20261014`.
Remote summaries are under `/data1/efs/deploy-85f2a199aa39` on Intel and
`/data1/efs/intel-client/deployment` on the external hosts; the initial xorinox
sample manifest remains under its original `deploy-85f2a199aa39` directory.
No raw logs, packet captures or generated payloads are committed.
