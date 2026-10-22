# orcd-test: dead-code cleanup and W127–W131 on a reformatted cluster — October 22

**Purpose:** first mounted, RDMA, `.efs`-enabled run of branch
`claude/dead-code-cleanup-5f7087` (the [dead-code audit](../reviews/dead-code-review-20261021.md)
cleanup, devel merged, and the W127–W131 apply hardening), and a restart on
data written by the previous build of the same branch.
**Verdict: PASS** for the suites below. Not a release gate; W127–W131 RDMA
fault gates and multi-host fault injection remain NOTRUN.

| | |
|---|---|
| Builds | `cfc25eeec` (cleanup + devel merge incl. D25), then `57ea88201` (+ W127–W131); both built on every host by `~/git/cluster/orcd-test.py deploy` |
| Cluster | orcd-test: fcstor003–006 storage (4 voters, `EFS_MD_RAFT_N=4`), fcstor007–013 clients; 6 NVMe roots per storage node; direct I/O |
| Transport | RDMA (`mlx5_2`) for daemons and mounts |
| Features | `EFS_COMMANDS_SERVER=1`, `EFS_COMMANDS=1` (`.efs` on every mount), `EFS_EXPIRY=0`, query refresh 60 s (default) |
| Stores | wiped and reformatted with `cfc25eeec` (user decision, October 22): the previous data was in the archived packing format (KV `MANIFEST` reader capability 1, refused by every build since D37) |

## Results

| Step | Build | Result |
|---|---|---|
| Fresh format, 4 nodes up, 7 mounts | `cfc25eeec` | PASS. 50 MB file written on fcstor007 reads back with the same SHA-256 on fcstor013; `.efs/stats` exact (201 files, 50,000,692 bytes) |
| New diagnostics under traffic | `cfc25eeec` | `io-stats` RDMA row live (49 conns, ~21k data frames each way, 0 credit stalls); `pack-compact` `packer:` line shows node 2 packing the 200 small files (`built=1 files_packed=200`) |
| POSIX core, 7 clients concurrently | `cfc25eeec` | 216/216 on every client (0 fail, 0 skip, 0 notrun) |
| Restart on existing current-format data | `57ea88201` | PASS: no refusal, no INCOMPATIBLE line; file hash unchanged from another client |
| POSIX core, 7 clients concurrently | `57ea88201` | 216/216 on every client |
| `tests/live/efs_commands.py` | `57ea88201` | PASS |
| `tests/live/efs_command_options.py` | `57ea88201` | PASS |
| `tests/live/efs_commands_load.py` | `57ea88201` | PASS: 144,827 files by 8 writers with 2 RPC page readers; every payload verified; converged in 55 s; create p50/p95/p99 4.8/33.7/40.8 ms |
| `tests/live/virtual_interfaces.py --freshness-s 150` | `57ea88201` | Full run with the fixed harness: 10/11 PASS (`retired_names`, `recovery`, `snapshot`, `stats`, `stats_mutations`, `invalid`, `permissions`, `name_limits`, `fd_capacity`). `nested_authorization` failed there with connection refused because that run was launched without `EFS_QUERY_TEST_NODES` (its RPC helper then dials consecutive local ports); the isolated run of that gate with the variable set PASSES |

The live harnesses needed fixes for the P5 page-only query path and for a
60 s refresh (they assumed localhost fixtures with fast refresh): see the
harness commits on the branch. They ran from a side copy of `tests/live`
with the deployed `efs-mgmt`; the deployed trees were not modified.
A first attempt of `efs_commands.py`/`efs_command_options.py` started
40 s after the restart failed at `.efs/stats` before the query catalog was
restored; rerun later, both passed.

## Reproduction

From the Mac, with `~/git/cluster`:

```sh
python3 orcd-test.py stop
python3 orcd-test.py deploy --source ~/git/efs --ref claude/dead-code-cleanup-5f7087
python3 orcd-test.py start --transport rdma --commands
python3 orcd-test.py init-export        # only on freshly formatted stores
python3 orcd-test.py mount --transport rdma --commands
```

On each client: `python3 -u tests/posix/posix_suite.py /tmp/efs-mount/posix-$(hostname -s) --suite core --jobs 16 --results posix-core.tsv`.
On fcstor007, after `EFS_COMMANDS=1 bash scripts/client.sh 172.16.223.57:17432 /tmp/efs-mount2`, with
`EFS_QUERY_TEST_PORT=17432 EFS_QUERY_TEST_HOST=172.16.223.57` and
`EFS_QUERY_TEST_NODES='{"1":["172.16.223.57",17432],"2":["172.16.223.58",17432],"3":["172.16.223.59",17432],"4":["172.16.223.60",17432]}'`:
`python3 -u tests/live/<harness>.py /tmp/efs-mount /tmp/efs-mount2` for each harness above
(`virtual_interfaces.py` with `--freshness-s 150`; `efs_commands_load.py` with a fresh `EFS_QUERY_TEST_FIXTURE`).

Raw logs stay on the cluster: `/home/erbmi1/orcd/scratch/efs/orcd-test/<host>/suite-cfc25eeec/`
and `suite-57ea88201/` (posix TSVs and harness logs), daemon logs in
`<host>/efsd.log`. Previous source trees are kept at `/tmp/efs-before-orcd-test-<rev>`.

## Limits

- One cluster, one run per suite; no fault injection, no node loss, no NFS.
- W127–W131 refusal itself was exercised on a private 3-node TCP set on
  node9901 (`scripts/w127_incompatible_entry.sh`), not on orcd-test,
  because a committed unsupported entry halts its group until reformat.
- The archived-format data that was on orcd-test before October 22 was
  deleted, not migrated (no backward compatibility).
