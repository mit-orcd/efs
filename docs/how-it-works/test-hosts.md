# Test hosts and the wave gate

[Testing](testing.md) · [Verification plan](verification.md)

How to reach the Linux test hosts, work on them without disturbing anyone,
and run the full gate. Script: [`scripts/wave_gate.sh`](../../scripts/wave_gate.sh).

## Access

User `erbmi1` behind the jump host `orcd-login`, through the shared mux socket:

```sh
ssh -l erbmi1 -o BatchMode=yes -o ControlPath=none \
  -o ProxyCommand="ssh -o ControlPath='$HOME/git/cluster/state/orcd-mini/.ssh-mux-erbmi1@orcd-login.mit.edu:22' -o BatchMode=yes -W %h:%p orcd-login" \
  <host> '<command>'
```

- If a password or token prompt appears, stop and ask the user. Never type one.
- Ship the tree as `git archive --format=tar -o <file>.tar HEAD`, copy it with
  `scp` through the same ProxyCommand (up to three attempts,
  `-o ConnectTimeout=30 -o ServerAliveInterval=15`) and compare `sha256sum` on
  both ends.
- No secrets and no account names other than `erbmi1` belong in the tree.

## Hosts

| Host | Use |
| --- | --- |
| fstor007 | 96 CPUs, no valgrind header. libfuse by shim (below). Wave gate host. |
| node9900 | 192 CPUs, no valgrind. `EFS_FUSE_PREFIX=/data1/efs/deps/libfuse`. Also carries the live orcd-mini client. Wave gate host. |
| node9901 | Live orcd-mini daemons. Use last. |
| fcstor003 to 015 | Private clusters only. fcstor015 refuses the key. |

The home directory is shared by all of these hosts. The libfuse shim on
fstor007 is `/tmp/efs-<name>-fuse` with `lib64` and `include` symlinks to
`~/orcd/scratch/efs/orcd-test/deps/libfuse/lib` and `.../include`;
`export EFS_FUSE_PREFIX` to it. The Makefile needs nothing else
([build page](../status/2026-11/fuse-prefix-build-20261109.md)).

## Never touch

On any host: `efsd` on ports 17432 to 17435, everything under `/data1/efs`
(read only), the `efs` user and its mounts, ports 27432 and 27433,
`/data1/efs/mnt`, `/data1/efs/orcd-mini-mnt`, the `efs-fuse` process of that
mount on node9900, the stores on fcstor003 to 006 and the `/tmp/efs-mount`
mounts on fcstor007 to 013. Stop only processes you started, by pid. Never
`pkill -f`. Private clusters on fcstor hosts: see the
[distinct-host recipe](../status/2026-11/k38-distinct-host-rdma-20261107.md).

## Work layout

- Tree `/tmp/efs-<name>`, `TMPDIR=/tmp/efs-<name>-tmp`, ports 18000 to 19999
  only, a name such as `taskT6` for a task.
- `SCREENDIR=$HOME/.screen` for `screen`. It is on the shared home, so
  `screen -ls` on one host lists other hosts' sessions as dead: never
  `screen -wipe`; match sessions by name and host.
- Raw output stays outside Git in `~/efs-artifacts/<name>-<yyyymmdd>-<host>/`
  with a `SHA256SUMS` file. Cite it from the evidence page, never commit it.
- Cleanup: delete trees with `~/bin/edelete --force --delete <dir>` (guard with
  `[ ! -e "$d" ] ||`, it returns 2 for a missing path), single files with
  `rm -f`; `rm -rf` only where `~/bin/edelete` is absent. Remove the tar too.
  `scripts/wave_gate.sh --clean <name>` does this for `/tmp/efs-<name>*`.

## A shipped tree has no `.git`

`git archive` leaves out `.git`. A script that calls `git` must also work
without it.

## The wave gate

Ship a tar of the tree to the host, then run **on the host**:

```sh
scripts/wave_gate.sh <host> <tar-on-host> <name>   # host: fstor007 or node9900
scripts/wave_gate.sh --clean <name>                # afterwards
```

(Copy the script from the tree you are testing, for example by extracting it
from the tar.) It extracts the tar to `/tmp/efs-<name>`, sets the libfuse
prefix by host, starts a detached `screen` session `<name>`, and the session
first waits until three checks 20 s apart find no other test process of the
user (`pgrep -u $(id -u) -af '/tmp/efs-task|make_test_summary|sanitizer_check'`,
ignoring its own tree and the orphan `efs-taskDQ/m2/summary` on node9900). The
20 s is the poll interval; `wait.log` in the artifacts directory records
`waiting:` lines with the load, then `start:` and `end:`. Then it runs
`EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh` and copies `summary.txt`,
`timings.tsv`, `build.log`, `progress.txt`, `gate.log` and `fail-*.log` with
`SHA256SUMS`. If the artifacts directory exists the script exits 2 and moves
nothing: rename a first run to `...-gate1` before a second run on a fixed
tree. Check `uptime` first; above load 20, wait and say so. The gate is
finished when `SHA256SUMS` exists.

First line of `summary.txt`:

| Status | Meaning |
| --- | --- |
| `COMPLETE` | every NOTRUN is a known host limit, nothing OVERBUDGET; counts as pass |
| `PASS` | no NOTRUN |
| `FAIL` | at least one failure (or build/enumeration failed); the names follow |
| `INCOMPLETE` | a NOTRUN that is not a host limit |

`OVERBUDGET n` on that line lists commands that exceeded their budget in
`tests/budgets.tsv`; it does not change the status. Never widen a budget.

A failure of a known intermittent test: keep the artifacts, run that test
alone three times on the same build, run the gate once more, and never widen
a budget.

Around the script, the merges require:

- Both fstor007 and node9900 end with `FAIL 0` on the same tree before
  `devel` is fast-forwarded. Host-limit NOTRUN rows are fine.
- A test that fails because `devel` changed under a branch is fixed in the
  merge tree and the gate is rerun on the new commit.
- The ASan and TSan lanes (`bash tests/sanitizer_check.sh asan` and `tsan`)
  run on a third host and are recorded. `k96_status_slots_live` currently
  fails under TSan (row K107).
