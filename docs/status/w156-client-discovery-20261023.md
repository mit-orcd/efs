# W156 — client discovery without inspecting other users' clients

**Purpose.** Work-queue item **W156** (review:
[devel integration review](devel-integration-review-20261022.md#evidence-retention-and-the-discovery-override)).
`scripts/client_processes.py` (used by `scripts/client.sh stop` and every
harness's checked stop) found `efs-fuse` processes by parsing argv. Another
user's client with a relative mount argument made it read that process's
`/proc/PID/cwd`, which a non-root user cannot, so discovery refused ("client
discovery requires root") and every non-root `client.sh stop` on a shared
host failed. The stopgap `EFS_CLIENT_SKIP_FOREIGN=1` (`2213a4120`) skipped
uninspectable foreign clients and proved nothing about a foreign client on
the same target. The reviewers asked for an independent proof of the target
mount and its control owner, refusal of ambiguous ownership, tests with
foreign clients on the same and on unrelated targets, and removal of the
switch.

**Verdict: PASS (code and tests); live foreign-client case NOTRUN.** The
synthetic fixtures cover every adversarial case and pass; `make test-all`
on nuc passes with no switch. The real foreign relative-argv client that
made the old code refuse on nuc was not running during this gate (nuc had
no `efs-fuse` and no `efsd` at all), so the real-host check ran with zero
efs-fuse mounts visible and does not exercise that case.

Branch `devel-w156`, not merged, not pushed.

## Change

Ownership never depends on another user's process.

- **Target mount and owner.** `/proc/self/mountinfo` names the mount
  (mount point, `fuse.efs-fuse`, device) and its owning uid (`user_id=`).
  A mount of another uid is refused for a non-root caller (`... is mounted
  by uid N, not uid M; refusing`); stacked efs-fuse mounts on one path are
  refused as ambiguous.
- **Controller.** The mount's control endpoint
  (`/tmp/efs-control-<fnv64(canonical mount)>.sock`, bound by efs-fuse
  after `fuse_session_mount` and closed before unmount; the channel
  `client.sh` drains) is looked up in `/proc/net/unix`. The controller is
  the single process the caller may inspect (its own uid; every uid for
  root, decided from the world-readable `/proc/PID/status`) whose
  descriptors include that socket inode, and it must be `efs-fuse` and hold
  `/dev/fuse`. Refused: endpoint file owned by another uid, endpoint held by
  no inspectable process, more than one holder, holder without a FUSE
  connection or of another program. No endpoint means no controller (stale
  mount, or a client that already closed its channel).
- **Foreign clients** are filtered by status uid only; their argv, cwd and
  descriptors are never read, so unrelated foreign clients are ignored
  whatever their argv looks like.
- **Detached own clients** (`list`, and `clients()` used by
  `devops/nuc-efs/start.sh`): an own `efs-fuse` holding an endpoint whose
  mount is no longer listed is named by the one argv entry (relative ones
  resolved against its own cwd) whose endpoint hash equals the held
  endpoint; otherwise `list` refuses.
- **Retirement.** `has MOUNT` prints the proven controller as
  `PID STARTTIME`; `client.sh` passes it to `retire MOUNT PID STARTTIME`,
  which signals only that process while it still holds the endpoint
  (pidfd), waits a bounded time for natural exit when it no longer does,
  and refuses when another process now holds the endpoint.
- **`list`** prints the caller's own efs-fuse mounts (including stale ones)
  and own detached clients; other users' mounts are no longer listed.
- `EFS_CLIENT_SKIP_FOREIGN` is deleted, with its uses in
  `scripts/gates_nuc_k01_stop.sh`, `scripts/gates_nuc_w148_campaign.sh` and
  `scripts/k14_atomic_cluster.sh` (no `w125_*` or K05 script set it).

Linux exposes no mapping from a `/dev/fuse` descriptor to its FUSE
connection, so the connection is bound to the process through the
per-mount endpoint plus the `/dev/fuse` descriptor, and to the owner
through `user_id=` and the endpoint file's uid.

## Tests

`tests/test_client_processes.py`, synthetic `/proc`, `mountinfo`,
`net/unix` and endpoint fixtures (any platform). A guard fails the test if
any foreign process's argv, cwd or descriptors are read.

| Case | Expected |
| --- | --- |
| own client on the target (also via `..` spelling) | found |
| foreign client, relative argv, unrelated mount | ignored, cwd never read |
| foreign client on the same (mounted) target | refused |
| foreign detached client on the same target (endpoint uid) | refused |
| endpoint held by no inspectable process | refused |
| stale mount with no controller; unknown path | none |
| two candidate controllers (mounted and detached) | refused |
| endpoint holder without `/dev/fuse`, or not `efs-fuse` | refused |
| stacked efs-fuse mounts | refused |
| own detached client named from argv (memcheck wrapper, relative) | found; unmatched argv refused |
| root caller, detached client of another uid | found |
| PID reuse / zombie | not the drained process |

Linux only, actual `/proc`: `list` on the real host succeeds and every
efs-fuse mount of another uid is refused and not listed; a synthetic
`efs-fuse` daemon (prctl name, `/dev/fuse` open, endpoint bound) is found
and retired by SIGTERM; one bound elsewhere is not a controller and is not
retired; one that closed its endpoint is waited out; with denied
descriptors or without `/dev/fuse` ownership is refused and the process
kept. `tests/test_client_stop.py` checks that `client.sh` hands the
identity from `has` to `retire`.

## Gate

- **Tested source:** `dca4dd544` on `devel-w156` (parent `757d76844`,
  devel), `git archive HEAD` copied to `nuc:~/efs-w156/src`, built there
  with `make -j8` (no changes on the host).
- **Host:** nuc, user `maik` (uid 1000), 8 CPU, Rocky Linux 10.2, kernel
  `6.12.0-211.67.1.el10_2`, Python 3.12.15, `/dev/fuse` present, libtsan
  installed; no RDMA device, no valgrind. The user's live cluster was not
  running (no `efsd`, no `efs-fuse`, no `fuse.efs-fuse` line in
  `/proc/self/mountinfo`); nothing outside `~/efs-w156` was touched and no
  17xxx port was bound.
- **Commands:**

  ```bash
  git archive HEAD | ssh nuc 'mkdir -p ~/efs-w156/src ~/efs-w156/tmp && tar -x -C ~/efs-w156/src'
  ssh nuc 'cd ~/efs-w156/src && make -j8 && TMPDIR=$HOME/efs-w156/tmp make test-all'
  ssh nuc 'cd ~/efs-w156/src && python3 tests/test_client_processes.py && python3 tests/test_client_stop.py'
  ```

  Cleanup: `ssh nuc 'rm -rf ~/efs-w156'` (done).
- **Results:** `make test-all` exit 0; no `^make(\[[0-9]+\])?: .*Error`
  line. `tests/test_client_processes.py`: synthetic proof PASS, host
  discovery PASS (0 efs-fuse mounts visible), endpoint-owner retirement and
  refusal PASS. `tests/test_client_stop.py` PASS. The `FAIL` words in the log
  are negative controls inside passing tests. NOTRUN lines are host-caused:
  RDMA (no IB device), valgrind absent, `test_stalled_compactor` (needs
  `EFS_FAULTS=1`), cluster stop/remount scripts (no `~/git/cluster` on nuc).
  On macOS only the synthetic part runs (the rest prints NOTRUN).
- **Baseline:** before the change, the same test on nuc failed with
  "client discovery requires root" whenever the foreign client ran
  ([gates-nuc](gates-nuc-20261022.md#harness-and-script-changes),
  [K30](k30-intermittent-tests-20261023.md)); not re-measured here because
  that client was absent.
- **Raw log** (outside Git): session notes folder
  `local_19ae04bd-0721-46f2-9b3d-34642bbc893d/w156-nuc-dca4dd544/test-all.log`,
  4,555 lines, sha256
  `6f0fa66d1ca897ac2c23183249f98eba61c7311b03eb48891280c1fff12eb62e`.
- **Remaining gate:** run `tests/test_client_processes.py` (or a checked
  `client.sh stop`) as a non-root user on a host where another user's
  `efs-fuse` with a relative argv is running (nuc once its cluster is up,
  or amd).

## Limitations

- Ownership proof relies on the control endpoint living in `/tmp` of the
  caller's mount and network namespace; a client in another namespace is
  not seen (as before).
- A client between `fuse_session_mount` and binding its endpoint is not a
  controller; `client.sh stop` still runs the control stop for a listed
  mount and refuses when it is unavailable (rc 3), unless
  `--force-discard`.
- The cluster scripts' tests (`tests/test_cluster_stop.py`) were NOTRUN on
  nuc; they mock `client_processes.py` and only use `list` and the `has`
  exit status, which are unchanged in form.
- `list` no longer prints other users' efs-fuse mounts; `cluster.sh stop`
  runs `client_processes.py` as the mounting user (`efs`), so its own
  mounts are unchanged.
