# AMD POSIX link of symlink failure

Status: corrected no-follow test merged from devel-roce-rdma; all four AMD I/O/transport full POSIX and peer reruns pass. Production hard-link behavior was not changed. Additional follow/dangling cases and comparable NUC Python/syscall evidence below remain useful test follow-ups. See [release acceptance](v020-amd-release.md).

## Finding

The `link_of_symlink` failure on AMD is a test/request mismatch, not a
confirmed EFS hard-link defect. `tests/posix/posix_suite.py:3584` says it tests
Linux `link()` without following the source symlink, but calls
`os.link(source, destination)` without `follow_symlinks=False`. On AMD's
Python build the default call issues:

```
linkat(AT_FDCWD, source, AT_FDCWD, destination, AT_SYMLINK_FOLLOW)
```

Following the target is the requested behavior. The test then incorrectly
requires the destination to be a symlink and reports a filesystem failure.
The preceding buffered and direct full runs each recorded 215 passes, this
one failure and one skip. The current deployed build is
`cd0cc4c589fa-dirty`; all four AMD daemons run `--direct-io`.

## Controlled evidence

A disposable fixture was run on both native `/tmp` and the EFS mount
`/home/efs/efs/mnt`, with syscall tracing. Each fixture was removed afterward.
Both filesystems produced the same results:

| Invocation | Observed syscall | Destination |
| --- | --- | --- |
| Python `os.link(l, h)` | `linkat(..., AT_SYMLINK_FOLLOW)` | regular target inode |
| Python `os.link(l, h, follow_symlinks=False)` | `linkat(..., 0)` | same symlink inode |
| Python `os.link(l, h, follow_symlinks=True)` | `linkat(..., AT_SYMLINK_FOLLOW)` | regular target inode |
| libc `link(l, h)` via ctypes | `link(...)` | symlink |

The explicit no-follow case returned the source symlink's inode, and the
follow case returned the target's inode. Evidence is retained in
[probe results](../../results/measure/20261008-amd-link-symlink/amd-link-symlink-probe.jsonl)
and [syscall trace](../../results/measure/20261008-amd-link-symlink/amd-link-symlink-syscalls.txt).
The evidence directory is dated by UTC; this was Oct 7 evening in EDT.

`efs_fuse_link_at` links the supplied inode and refuses directory links;
`ll_link_run` passes that inode through and replies with it. Neither performs
source-path symlink traversal. The kernel resolves the source according to
the link/linkat flags before handing the inode to FUSE. Do not change EFS to
second-guess or reverse that resolution.

## Required fix

1. Change this test's invocation to
   `os.link(source, destination, follow_symlinks=False)`. Keep its inode,
   nlink, readlink and target-read assertions. Clarify that it exercises
   no-follow hard linking; Python's wrapper need not issue raw `link()`.
2. Add an explicit follow case with `follow_symlinks=True`: destination is
   regular and shares the target inode/data, while the source remains a
   symlink. Test semantics, not a platform-dependent default wrapper choice.
3. Add a dangling-source-symlink case: no-follow links the symlink and retains
   its text; follow returns ENOENT. Verify unlinking one hard-link name leaves
   the other valid and link counts correct. Check peer visibility from mnt2.
4. If testing the raw Linux `link()` syscall remains a requirement, make that
   a separate Linux-specific case with an explicit syscall/libc invocation.
   Avoid inferring its semantics from Python's default `os.link` call.

## Acceptance before closure

Run the corrected focused test on native storage and AMD EFS, then both full
POSIX suites after a fresh client restart. Require 216 passed, zero failed,
one existing unsupported skip for the single-client suite and 64/64 for the
current peer suite (account separately for any added cases). Retain the exact
source/build identity, mode and results; a test change must not be recorded as
a production EFS repair. Repeat on NUC and document Python/syscall differences
before using historical clean NUC results as a comparable gate.

Most recent pre-fix AMD direct runs: full POSIX 18.4 seconds, 215/1/1;
two-client 7.5 seconds, 64/64. Buffered runs showed the same test failure.
