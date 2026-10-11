# W108 · Resolve owner names in directory-local long listings

**Status:** implemented and candidate-gated; rebuilt-client rollout pending — now in the `devel-orcd` queue after the first-priority packing work, under the user's explicit ORCD ownership authorization (October 11, 2026).

## Report and cause

The user reports `/tmp/efs-mount/.efs/ls -l` on ORCD fcstor007 displaying
`246111:246111`; UID 246111 should display as `erbmi1` when resolvable.

In the deployed Oct 11 baseline, `src/client/commands/command.py`
`render_entry()` formats ownership with
`%d:%d` directly from the snapshot's numeric `uid` and `gid`. The command
imports neither `pwd` nor `grp` and performs no account-name resolution.
Its `-l` help explicitly describes `uid:gid`. Generated
`src/client/commands/source.h` embeds that same implementation into the
virtual executable. Numeric output is therefore the current formatter's
behavior, not evidence of incorrect stored ownership or failed NSS resolution.
Read-only live confirmation on fcstor007 on October 11:

- `id`: UID and GID 246111 both resolve to `erbmi1`.
- `getent passwd 246111` and `getent group 246111`: both return `erbmi1`.
- `stat -c '%u:%g %U:%G' /tmp/efs-mount/001`:
  `246111:246111 erbmi1:erbmi1`.
- Reading `/tmp/efs-mount/.efs/ls` confirms the deployed `%d:%d` formatter.

This proves account resolution works on the executing host and the virtual
command bypasses it. No deployment or ownership changes were made.

## Required follow-up

Resolve UID and GID independently on the machine executing the command using
its account database/NSS (`pwd.getpwuid` and `grp.getgrgid` or equivalent).
Use user/group names by default in human-readable long listings; fall back to
the original numeric identifier when a name cannot be resolved. The group name
must come from the group database, not be inferred from the user name.

Keep stored ownership, permission checks, query snapshots and JSON UID/GID
fields numeric. Add an explicit numeric-output option such as
`-n`/`--numeric-uid-gid`, document its interaction with `-l`, and keep shared
`.efs/find -l` formatting consistent. Cache distinct UID/GID resolutions per
invocation to avoid repeated NSS calls for large listings. Escape resolved
names with the existing safe text rules. Regenerate `source.h` from the Python
source; do not edit only the generated header.

## Gates

- ORCD fcstor007: compare `id`, `getent passwd 246111`, `getent group 246111`,
  native `stat`/`ls -l`, and the deployed virtual command source/output.
- Test known user/group names, unknown IDs, independent user/group fallback,
  explicit numeric mode, unchanged JSON ownership and safe name rendering.
- Verify shared find/ls and export formatting, bounded repeated resolutions,
  and deploy a rebuilt client before checking the virtual executable.

The reported `refresh error -13` is a separate snapshot-refresh issue. Owner
name formatting does not explain or fix it; retain that diagnostic separately.

## Implementation checkpoint

`devel-orcd` now resolves names by default in shared ls/find long formatting,
with independent UID/GID numeric fallbacks, safe text escaping, and an
invocation-local LRU bounded to 2,048 identities. Unknown IDs are cached too.
`-n` / `--numeric-uid-gid` implies a long numeric listing and performs no NSS
lookup. JSON fields remain numeric. Embedded `source.h` was regenerated.

All 29 command regressions pass, including independent fallback, name escaping,
repeated-lookup caching, its bound, numeric mode, and source/header identity.
A private candidate source on fcstor007 reads the actual mounted query snapshot:
`-l` prints `erbmi1:erbmi1`, `-n` prints `246111:246111`, and JSON retains numeric
246111. [Live candidate output](../../results/measure/20261011-orcd-tiny-packing/w108-candidate-live.txt).
This does not replace the running embedded command. Keep W108 open until the
rebuilt clients serve these bytes and shared find/ls live gates pass.
