# Intel 4-node cluster: fresh deploy of `devel-testing` and all POSIX suites — 2026-10-23

**PASS: every POSIX suite at 100% of its supported cases; the only skip is
shared writable mmap, unsupported by specification (W104).** User request:
branch `devel-testing` from devel, wipe and redeploy the Intel cluster with
new binaries, mount it from xorinox with a matching client, run all POSIX
suites and fix failures until they pass. No EFS product defect was found.
One devops harness defect was found and fixed (below). No backward
compatibility (AGENTS.md).

## Build and deploy

- **Source:** branch `devel-testing` at `163241584` (= devel at branch time).
  The build identity is `163241584306`, version
  `v0.3.2-pre-alpha-1041-g163241584`, built 2026-10-23T03:39:42Z.
- **Intel hosts:** built by devops `EFS_CLUSTER=intel ./deploy.sh <worktree>/`.
  - **intel1:** efsd `b9842b943d190c1eca0f6c7875d5a6f4e24f2ab795016da4b7b59fde6620d8f7`,
    efs-fuse `7961cb176c4b07c516b40d24ee264327913363ca47ff64752312fe70d103f587`.
  - **intel2:** efsd `cfe7a08d984c11d67961c28737c84e95ed47289bdd583e10f6f742b85dfe0b0c`,
    efs-fuse `84133612ddea34809a8854ae7a08ab849b7943b11b4aa0ff0282a37974ef8c68`.
  - The two hosts compile the same source separately with `-march=native`,
    so their bytes differ; status reports 4/4 identical versions.
- **xorinox client:** built in `/data1/efs/intel-client/releases/163241584306`
  (`make efs-fuse efs-mgmt`, same identity), efs-fuse
  `41df0484302894abdedaa67c9f587e61ed5a329120de60b4ff67c8cb18504d74`.
- **Wipe (user-authorized):**
  1. The external clients on xorinox and amd were stopped, then devops
     `stop.sh`; nothing was using the mounts (`fuser`).
  2. Stores n1–n4 (`/data1/efs/n1`, `/data2/efs/n2`, `/data1/efs/n3`,
     `/data2/efs/n4`; `/data2/efs` links to `/home/efs/additional-work-dir`)
     and `.mkfs-done` were deleted with `edelete`: 12.8 M files, 0 errors.
  3. `start.sh` formatted a fresh cluster.
- **Topology:** four efsd: intel1 10.0.0.207 holds n1/n2 and intel2
  10.0.0.209 holds n3/n4, on ports 17432/17433.
- **Configuration:** TCP, direct I/O, 400 GiB quota per node.
- **`.efs`:** on, with `EFS_COMMANDS=1 EFS_COMMANDS_SERVER=1 ./start.sh`;
  Intel's default profile is off.
- **Clients:** `/data1/efs/mnt` on both Intel hosts, and xorinox
  `/data1/efs/intel-mnt`.

## Results (second full run, 04:28–04:41Z)

| Suite | Command | Result |
| --- | --- | --- |
| Core POSIX, intel1 mount | devops `smoke.sh` (`posix_suite.py`, jobs 1) | 216/216 PASS |
| Two-client across hosts | devops `smoke2.sh` (`posix_2client.py`, intel1 ↔ intel2) | 68/68 PASS |
| Durability across remount | devops `smoke-persist.sh` (`posix_persist.py`) | 27/27 prepare + 27/27 verify PASS |
| Extras, intel1 | `posix_extras.py /data1/efs/mnt --jobs 1 --timeout-s 150` | 4/5 PASS, 1 SKIP (`mmap_write_read`, W104) |
| Core POSIX, xorinox mount | `posix_suite.py /data1/efs/intel-mnt --jobs 1` | 216/216 PASS |
| Extras, xorinox | `posix_extras.py /data1/efs/intel-mnt --jobs 1 --timeout-s 150` | 4/5 PASS, 1 SKIP (`mmap_write_read`, W104) |
| Expiry lifecycle (private four-node fixture on intel1, ports 24410+) | `posix_expiry.py M1 --suite expiry --root A --root B --peer-mount M2 --jobs 1` | 134 checks PASS, 0 FAIL; its core run 216/216 |

The first full run (04:13–04:25Z) differed only in extras: 1/5 PASS with
4 SKIP, three of them `.efs` cases. The cause was the harness defect
below; the product was not at fault.

## Defect found and fixed

**devops `smoke-persist.sh` dropped `.efs` across its remount** (devops
`ff9701f`).
- **Cause:** the suite unmounts and remounts the client with the shell's
  default `EFS_COMMANDS=0`. A cluster started with `EFS_COMMANDS=1` therefore
  lost `.efs` after the persistence suite, and every later suite saw it
  absent.
- **Fix:** it now reads `EFS_COMMANDS` from the running client's environment
  and reuses it. The second run logged "remount keeps the running client's
  EFS_COMMANDS=1" both times.
- **README:** the same commit corrects the claim that Intel defaults to
  `EFS_COMMANDS=1`; `a45dbfc` made it 0.

## Operational changes outside Git

- **xorinox:** `/data1/efs/intel-client/client.sh` now takes
  `EFS_COMMANDS`/`EFS_COMMANDS_SERVER` from the environment, defaulting to 0
  as before. The previous file is kept as `client.sh.bak-20261023`. The
  `src` link points to release `163241584306`.
- **amd:** the Intel export client (`/data1/efs/intel-mnt`) was stopped for
  the wipe and left stopped. Its deployed release predates the export-label
  removal (`74c283293`) and needs a new client build before it can mount
  this cluster.
- **Gate runs:** the K1/T2 session ended its intel1 runs, and the K15
  session stopped its intel2 gates early so the intel start check would
  pass. Their own records cover what they ran.

## Reproduce

From the Mac with `~/git/devops/nuc-efs`:

```bash
EFS_CLUSTER=intel ./stop.sh            # after stopping the external clients
EFS_CLUSTER=intel ./deploy.sh /path/to/efs/checkout/
# wipe: edelete n1..n4 store dirs and /data1/efs/.mkfs-done on both hosts
EFS_CLUSTER=intel EFS_COMMANDS=1 EFS_COMMANDS_SERVER=1 ./start.sh
EFS_CLUSTER=intel EFS_COMMANDS=1 EFS_COMMANDS_SERVER=1 ./smoke.sh
EFS_CLUSTER=intel EFS_COMMANDS=1 EFS_COMMANDS_SERVER=1 ./smoke2.sh
EFS_CLUSTER=intel EFS_COMMANDS=1 EFS_COMMANDS_SERVER=1 ./smoke-persist.sh
```

Run the extras, the xorinox suites and expiry with the commands in the
table. `start.sh` refuses while any non-`efs` EFS process runs on a host,
which includes other users' private test clusters. Raw outputs stay outside
Git on the driver Mac (session scratchpad `intel-run/r1`, `intel-run/r2`)
and in each host's `/data1/efs/logs`.

## Limitations

- **mmap:** shared writable mmap remains unsupported (W104); its SKIP is by
  specification, not a pass.
- **Transport:** TCP only; the Intel hosts have no RDMA.
- **Not run:** `make test` and ior were not part of this request.
