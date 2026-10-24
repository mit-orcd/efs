# Alpha 1.0 docs review: code, test and docs findings fixed (Oct 24)

**Verdict: PASS (scoped).** On amd at `f588f6efd`, `make test-all` (461
commands) fails only the 11 commands that also fail on the untouched base
`376369aa8`: RDMA on the software `rxe0` device (9), the
`test_create_retry_fault` 30 s timeout, and `test_get_integrity.py`, which
does not compile on the base either (`read_reprobe_down` undeclared in the
extracted `read.c` functions, from the READ_VERIFY and K34 merges). The new
F14 test is red on the base (the 17th joiner never exits) and green on the
branch. Branch `claude/devel-sdocs-fixes-f02c43`, not merged, not pushed.

Fixes for the open findings of the
[alpha 1.0 docs review](s-docs-alpha-1.0-20261024.md) (F2–F6, F11, F13, F14,
F18–F20). F7, F8 and F22–F24 were decided or fixed elsewhere; F12 (no
authentication on daemon ports) is the user's scope decision and is
unchanged. The findings table there now has an outcome per row.

- **Purpose:** alpha 1.0 Stage 2 "one supported behavior" (A6–A12, D42):
  every command has help, one way to add a node, no command whose effect is
  silently lost at restart, specified behavior at the member limit, the S3
  operator workflows covered by tests, user docs that match the code.
- **Tested source:** `f588f6efd` (all code, tests and user docs; the next
  commit adds this page, the findings outcomes, the status README entry and
  the regenerated architecture docs). Base: `376369aa8` (`merge-wave5`:
  devel plus READ_VERIFY, K34, the A7 removals and K35). Both copied to amd
  with `git archive` (no `.git`; `EFS_GIT_ID` set to the commit).
- **Host:** amd (32 CPUs, Linux 7.0, ext4 home, TCP; software RDMA device
  `rxe0`, FUSE available), private trees under `~/sdocs-gate/`, `TMPDIR`
  inside each tree. Only loopback ports the tests chose were used; no
  cluster, mount or `efs` user was touched.
- **Raw outputs:** `amd:~/efs-artifacts/sdocs-fixes-20261024/` with
  `SHA256SUMS` (`SHA256SUMS` sha256 `a1ed1a7f…3b7a`, 55 files, 364 KB): runner output, `summary.txt`, `progress.txt`,
  `timings.tsv` and every failure log of each gate run, and the logs of the
  F14 A/B runs. The trees were deleted after the run.

## Outcomes

| # | Finding | Outcome |
| --- | --- | --- |
| F2, F3 | `efs-mgmt` commands without help | **Fixed.** The overview lists `sync-status`, `data-commit`, `pack-compact` and `raft-step-down`; each of them and `raft-resync` answers `COMMAND --help` with its syntax (notes for `pack-compact`, `raft-resync`, `raft-step-down`). New `make test` check `tests/test_cli_surface.py`: the commands `main` dispatches, the overview entries and the argument specs are the same set (33 in the product build), and every one answers `--help` with exit 0 |
| F4 | `add-node` | **Removed.** `efs-mgmt add-node` is refused (exit 2, `start the new node with efsd --join [SEED] STORAGE`), also with `--help`; `cmd_add_node`, the `EFS_MSG_JOIN` handler, `struct efs_msg_join` and the reply codes are gone; IDs 16/17 are retired (refused before dispatch, connection closed). `server_join_cluster` stays: auto-rejoin and `efsd --join` use it |
| F5 | `efs-fuse --bench*` | **Removed from the product binary.** Nothing outside the binary used it: no test, script, `~/git/devops` or `~/git/cluster` file names `--bench`, `--bench-time` or `--bench-mib` for `efs-fuse`. The in-process ladder (cpu/put/write levels, about 480 lines) is deleted; the flags are refused (exit 2, `measure the client with efs-bench or through a mount`) instead of reaching libfuse. `efs-bench` keeps the network, store and local engine benchmarks |
| F6 | `--no-persist` | **Done by A7** (confirmed): `efsd --no-persist` is refused (`--no-persist was removed (D42): membership is always persisted`, `efsd.c`), covered by `tests/test_direct_io_required.py`; `operations.md` and the surface list say so |
| F11 | `add-storage` not durable | **Removed; a node's roots are fixed at creation.** See [below](#f11-why-removal-and-fixed-roots) |
| F13 | POSIX cases | **Added** `dir_rename_noreplace_free_name` (`RENAME_NOREPLACE` onto a free name: `EOPNOTSUPP`, source unchanged, no target) and `mknod_special_files_refused` (FIFO, socket, character and block device: `EPERM`, no name left; regular `mknod` works) to `tests/posix/posix_suite.py` (core: 218 cases). Both run on a loopback mount in `tests/test_operator_workflows.py` (with the existing `dir_rename_noreplace`): 3/3 PASS. The [POSIX subset](../using/alpha-1.0-posix.md) P3 and P6 rows name them |
| F14 | 17th joiner | **Specified and tested.** A formed cluster of 16 members answers a joining store's HELLO with the existing `EFS_HELLO_REJECT_FULL`; the member logs `joining store … refused: the cluster has the maximum of 16 members`, the joiner logs `Join refused by … the cluster has the maximum of 16 members (16 now)` and exits 1. Before, the member queued an addition that `efs_membership_plus` refused, and the joiner retried forever. `tests/test_member_limit.py` (`make test-long`) |
| F18 | README unfinished list | **Corrected:** unfinished are verified recovery after an unclean stop (K36), online member removal, rebalancing and port authentication; session fencing (K5) and W154 rebuild on return are in place. The feature line says direct (`O_DIRECT`) fragment I/O (buffered I/O went with A7) |
| F19 | Quick-start store location | **Fixed:** the stores go under `${EFS_DEMO_DIR:-$HOME}`, with the automatic quota; the page states the requirements (a filesystem with `O_DIRECT`, free space above the 20 % / 1 GiB reserve) and why a small tmpfs `/tmp` fails |
| F20 | Workflow tests | **Added** `tests/test_operator_workflows.py` (`make test-long`): fixed storage roots and the removed commands; `raft-resync` refused on the group leader, accepted on a follower (`resync=1` in `raft-status`), and the follower applies to the leader's commit after 100 mount operations. Node addition is `efsd --join`, covered by `tests/test_k33_growth.py` and `test_k33_autoid.py` |

### F11: why removal and fixed roots

`add-storage` attached a root to the running daemon only: nothing recorded
it, the automatic quota was not recomputed, and the attach skipped startup
work for the new root (the put-sync durability state and the inventory).
Looking for the smaller correct design found a worse problem: packed
small-file segments are placed by `segment id % root count`
(`segment_store.c` `sg_root`). Fragments are found by probing every root,
but a segment is looked up only on the root its id maps to, so any change
of a node's root count, live or by restarting with a longer `STORAGE`
list, hides existing packed segments. Nothing checked that the roots given
at a restart were the node's roots.

A durable `add-storage` would need a recorded root list, a restart path
that reconciles it with the command line, the per-root startup steps on a
running node, and a segment placement that survives a new count (a
recorded root per segment or a probe). The smaller correct design is the
reverse: no change of roots at all.

- `efs-mgmt add-storage` is removed and refused (exit 2, `a node's storage
  roots are fixed when its store is created`); `EFS_MSG_ADD_STORAGE` (41/42)
  is retired; `server_add_storage_paths`, `server_storage_files_attach` and
  `server_writer_set_npaths` are deleted.
- Each root's existing 128-byte marker (`.efs-engine`) now records the root
  set (root 0's uuid), the root's index and the root count, written once
  when every root is fresh. A start whose roots do not match is refused
  before anything is written: a new root among marked ones (`1 of 3 roots
  are new`), a missing or moved root (`is root 2 of 2 of its root set; this
  command gives it as root 1 of 2`), or a marker without a root set (an
  earlier build: `format a new store`, no migration).
- More capacity means another node (`efsd --join`). The guide,
  `operations.md`, `power-user.md` and the surface list say so.

`tests/test_store_durability.c` (`test_root_set`) checks the marker rules on
real directories; `tests/test_operator_workflows.py` checks them on efsd:
added, dropped and swapped roots refused with the messages above, markers
unchanged, the new root left unmarked, the same two roots start again.

## Gate

Command for each tree, in a `screen` on amd:

```bash
cd ~/sdocs-gate/gate-branch && EFS_GIT_ID=f588f6efd EFS_TEST_JOBS=8 ./scripts/make_test_summary.sh
cd ~/sdocs-gate/gate-base && EFS_GIT_ID=376369aa8 EFS_TEST_JOBS=8 ./scripts/make_test_summary.sh
```

| Tree | Commands | FAIL | NOTRUN | Time |
| --- | ---: | ---: | ---: | ---: |
| base `376369aa8` | 458 | 11 | 5 | 933 s |
| branch, first run `de2ba2cec` | 461 | 13 | 5 | 463 s |
| branch `f588f6efd` | 461 | 11 | 5 | 595 s |

The branch adds `test_cli_surface.py` (`make test`), `test_operator_workflows.py`
and `test_member_limit.py` (`make test-long`): 458 + 3 = 461, all PASS. The
first branch run (`de2ba2cec`, since amended into `f588f6efd`, not on the
branch) also failed `test_retired_messages.py` (its
list of retired IDs lacked 16/17 and 41/42) and `test_recorder_stop.py` (it
required the removed `efs-fuse` bench recorder); both tests were updated in
`f588f6efd`, which changes nothing else. The 11 FAILs of the gated run, each
also failing on the base with the same message:

- `tests/test_rdma_xprt`, the seven `tests/test_rdma_segmented` cases and
  the RDMA part of `tests/test_declaration_gate`: `server_accept` /
  `efs_rdma_client_upgrade` fail on `rxe0` (host limit, as in the A6 and A7
  gates).
- `tests/faults/test_create_retry_fault.py`: the probe times out after 30 s
  (as in the A6 and A7 gates).
- `tests/test_get_integrity.py`: the extracted `read.c` code calls
  `read_reprobe_down`, which the test does not extract (`-Werror`
  implicit declaration). A base defect from the merges; not in this scope.

NOTRUN on both: `test_cluster_stop.py` (`~/git/cluster` absent),
`test_stalled_compactor` (needs `EFS_FAULTS=1`), the leak gate (no
valgrind), async durability (TMPDIR not XFS), and the runner's own NOTRUN
fixture.

**F14 A/B** (`tests/test_member_limit.py`, copied into the base tree): base
`376369aa8` formed 16 members in 8.1 s, then the 17th joiner was still
running after 60 s (`TimeoutExpired`, FAIL); branch `f588f6efd` PASS in
14.0 s (and 14.4 s in an earlier standalone run).

## Reproduce

1. `git archive <commit> | ssh amd 'mkdir -p ~/t && tar -x -C ~/t'`, then on
   amd `cd ~/t && EFS_GIT_ID=<commit> EFS_TEST_JOBS=8 ./scripts/make_test_summary.sh`
   (about 20 minutes; loopback ports only).
2. Single tests after `make -j32 all tests/faults/efsd`, with `TMPDIR`
   inside the tree: `python3 tests/test_cli_surface.py` (under 1 s),
   `python3 tests/test_operator_workflows.py` (about 10 s, FUSE for steps
   2–3), `python3 tests/test_member_limit.py` (17 daemons, about 15 s).
3. Delete the tree with `edelete --force --delete ~/t`.

## For the parent (outside the repository)

- No script in `~/git/devops` or `~/git/cluster` uses `efs-mgmt add-node`,
  `add-storage` or `efs-fuse --bench*` (searched, `ACCOUNTS.txt` excluded).
- Stores formatted by an earlier build have root markers without a root
  set and are refused by this build (`format a new store`): deploy it on
  freshly formatted stores, as for the placement change.
- A node's `STORAGE` list must not change between restarts; launch
  configurations that build it from a host inventory must keep its order.

## Limitations

- The F13 cases ran on one loopback mount on amd, not on the recorded
  multi-host POSIX runs; the full core suite was not run on this commit.
- F14 is tested with 17 daemons on one host; multi-host NOTRUN.
- `raft-resync` is checked on a healthy follower (accepted, keeps
  following); a follower with a conflicting log, the repair case, is only
  covered by the RDMA script `scripts/w145_deposed_leader.sh`.
- Fixed roots mean a failed disk under one root of a multi-root node cannot
  be replaced in place: the node is formatted again and joins under a new
  id (online removal of the old id does not exist in alpha).
