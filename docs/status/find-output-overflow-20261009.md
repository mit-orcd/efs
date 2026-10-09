# W92 large find result refusal — October 8, 2026 New York

User requested merging devel into `devel-find-stats`, then investigating:

```text
/data1/efs/mnt/.efs/find .txt
efs command: [Errno 75] Value too large for defined data type: '/data1/efs/mnt/.efs/.query/v1.find.3.2e747874'
```

The clean branch fast-forwarded from `befaee95` to devel
`ab050595c3b8ac21f8b5880a243f81c1a10649e4`. The installed NUC source is already
that exact devel revision. This investigation does not deploy another build,
stop the active tiny-file generator or alter existing test trees.

## Confirmed cause

The search exceeds the **16 MiB serialized JSON result budget**. This is an
explicit completeness-preserving refusal, not a file-size overflow or observed
storage corruption. The command fetches metadata-rich JSON internally even
when its display is plain paths. Selecting `--json`, `-0` or `--relative` does
not remove that underlying response limit. `.txt` is a literal contains match.

At measurement, root stats reports **992,269 files / 4,241 directories**,
254,705,611 logical bytes, a complete snapshot and `refresh_error=0`. The
workload continues to grow, so these are snapshot counts rather than a frozen
live namespace census.

Raw requests use the actual efs account's UID/GID, root inode 1, command wire
version 2 and a 1,000,000 candidate budget. All failed replies carry zero text:

| Query | Response cap | Status | Checked directories | Response bytes | Matches |
| --- | --- | --- | --- | --- | --- |
| Contains `.txt` | 256 KiB | -22, EFS_ERR_OVERFLOW | 17 | 0 | No partial result |
| Contains `.txt` | 1 MiB | -22 | 27 | 0 | No partial result |
| Contains `.txt` | 16 MiB | -22 | 347 | 0 | No partial result |
| Exact `file_000000.txt` | 16 MiB | Success | 4,224 | 475,857 | 2,149 |

These four probes use one published generation (461). Increasing only the
output allowance advances further before the refusal. The 347 checked
directories are well below the separate 8,192-entry authorization cache bound.
The candidate-visit budget returns BUSY on exhaustion, not this -22 status.
The normal command reproduces the corresponding Linux EOVERFLOW/errno 75.
A `.txt` query scoped to `tiny-devel-ab050595/folder_000001` succeeds through
FUSE, returning 46,282 JSON bytes with complete status and fresh source age.

## Code path

- `include/efs/protocol.h`: `EFS_QUERY_MAX_OUTPUT = 16u << 20`;
  `EFS_QUERY_MAX_VISITS = 1000000`.
- `include/efs/command_path.h`: command requests select those budgets.
- `src/server/raft_host.c`: `query_emit` serializes raw relative paths as hex
  with mode, UID/GID, size and mtime. `query_append` refuses an append past the
  requested result budget. `server_raft_host_query_snapshot` sets error replies'
  `text_len=0`, preserving the no-silent-truncation contract.
- `src/client/efs_fuse.c`: `find_query_ensure` maps the server overflow to
  EOVERFLOW during lookup/open of the hidden query inode.
- `src/client/commands/command.py`: the generic OSError handler exposes errno
  and the internal query pathname, losing the useful query-limit explanation.

The `.query/v1...` filename identifies the command-path encoding; it does not
mean the server returned the old v1 newline-only payload. The probe uses
command JSON wire version 2, as the corresponding path parser requests.

## Follow-up needed in W92

1. Add actionable error reporting: explain that the complete query exceeded a
   query limit and suggest a narrower containing directory or more selective
   literal/predicates. Distinguish output, authorization and visit/deadline
   limits rather than assuming every EOVERFLOW has the same cause.
2. Support large complete queries with bounded, versioned paging over an
   immutable published catalog. Bind continuation to query/scope/credentials
   and snapshot identity; refuse expired/restarted snapshots explicitly. Retain
   fresh authorization, mutation checks and per-page byte/visit/time bounds.
3. Preserve complete-output and sort semantics. Default client-side sorting
   currently requires collecting all entries; a large-result implementation
   needs bounded spooling/external sorting or a defined server ordering. Never
   silently combine pages from different publications or label a partial query
   complete. Do not fall back to a client tree walk or rebuild on command reads.
4. Add a deterministic multi-page fixture crossing 16 MiB, exact once-only path
   oracle, byte/count verification, seek/open immutability, concurrent mutation,
   chmod, refresh, restart and explicit resource-limit failure checks. Then run
   it against the loaded NUC namespace and both mounts.

Simply raising the cap is not a scalable fix: the server and FUSE response/
cache/concurrency budgets are also finite, and million-file metadata can grow
far beyond one response. The immediate workaround is a directory-scoped query,
for example:

```sh
/data1/efs/mnt/tiny-devel-ab050595/folder_000001/.efs/find .txt
/data1/efs/mnt/.efs/find file_000000.txt --match exact
```

No runtime fix is implemented or claimed by this investigation.
[Raw evidence](../../results/measure/20261009-find-output-overflow/).
