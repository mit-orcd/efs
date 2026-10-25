# Alpha 1.0 — POSIX subset (draft)

**Draft, October 24, 2026, revised October 25; checklist item S2.** The
supported operations and every known deviation of a Linux EFS FUSE mount,
derived from the suites in `tests/posix/` and their recorded results, and
checked against the client code at `ec3e09306`
([audit](../status/docs-audit-20261025.md)). A deviation is either a limitation of alpha (listed in
the [release notes draft](../release-notes/alpha-1.0.md)) or a gap still to
fix; none counts as PASS.

[Alpha guide](alpha-1.0-guide.md) · [Supported surface](alpha-1.0-surface.md) ·
[Testing](../how-it-works/testing.md)

## Suites and recorded results

| Suite | File | Cases | What it covers |
| --- | --- | --- | --- |
| Core | `tests/posix/posix_suite.py` | 218 | One mount: files, directories, names, links, attributes, permissions, errors, locks, sync, `O_DIRECT`, sparse files, concurrency, optional calls |
| Extras | `tests/posix/posix_extras.py` (`--suite extras`) | 5 | mmap and the `.efs` commands |
| Two-client | `tests/posix/posix_2client.py` | 68 | Visibility, races, locks and overlapping writes across two mounts on different hosts |
| Durability | `tests/posix/posix_persist.py` | 27 | Prepare, orderly unmount and remount, then verify bytes and metadata |
| Expiry | `tests/posix/posix_expiry.py` | `d5-expiry` | Expiry lifecycle on a private four-node fixture: the cluster expiry policy off by default, on/off at runtime, folder coverage, restarts and member loss, physical reclamation, then the core suite with expiry on (D43) |

The most recent recorded runs (both before the base commit; nothing has been
recorded on `ec3e09306` itself, and the C1 runs on 3, 4 and more than 4
distinct hosts are NOTRUN):

| Run | Build | Core | Extras | Two-client | Durability |
| --- | --- | --- | --- | --- | --- |
| [orcd-mini, Oct 24](../status/orcd-mini-deploy-cb970ca7c-20261024.md): three daemons on one host, RDMA, direct I/O | `cb970ca7c` | 216/216 | 4/5, 1 SKIP | 68/68 | 27/27 |
| [Intel, Oct 23](../status/intel-devel-testing-posix-20261023.md): four daemons on two hosts, TCP, direct I/O | `163241584` | 216/216 | 4/5, 1 SKIP | 68/68 | 27/27 |

The one SKIP in both runs is `mmap_write_read` (writable shared mmap,
W104). One known failure exists outside these runs: W159 (below).
The two F13 cases added after these runs (`dir_rename_noreplace_free_name`,
`mknod_special_files_refused`) run on a private loopback mount in
`tests/test_operator_workflows.py` (`make test-long`); see
[the F-findings fixes](../status/s-docs-fixes-alpha-1.0-20261024.md).

## Supported operations

Each line is covered by the named suite cases, all PASS in both runs above.

- **Regular files:** create, open (`O_CREAT`, `O_EXCL`, `O_TRUNC`, `O_APPEND`,
  `O_NOFOLLOW`, `O_DIRECTORY`), read, write, `pread`/`pwrite`, `readv`/`writev`,
  seek past end, holes and sparse files, overwrite, append, truncate and
  `ftruncate` up and down, zero-length writes, multi-chunk files and chunk
  boundaries (`basic_*`, `trunc_*`, `sparse_*`, `content_*`, `writev_readv_chunk_straddle`).
- **Directories:** `mkdir` (including `-p` and umask), `rmdir`, `readdir`
  with `.` and `..`, hidden names, many entries, nesting deeper than 64 levels,
  `mkdirat`/`unlinkat`, `openat` (`dir_*`, `mkdirat_unlinkat`, `opt_openat`).
- **Names:** any byte except `/` and NUL, control and high bytes, NFC and NFD
  forms side by side, case-sensitive; a component longer than 255 bytes fails
  `ENAMETOOLONG`, as does a path beyond `PATH_MAX` (`names_*`, `case_sensitive_names`).
- **Rename:** files and directories, within and across directories, over an
  existing file or empty directory, into a subdirectory, at the export root
  (`dir_rename_*`, `root_rename_*`, `rename_*`).
- **Links:** hard links (shared data, `nlink`, unlink of one name, across
  directories), symbolic links (relative, absolute, dangling, chains, to
  directories), `link` of a symlink (`hardlink_*`, `symlink_*`, `link_*`).
- **Unlink while open:** an open descriptor keeps reading and writing the file
  after its last name is gone (`unlink_open_file`, `nlink_after_unlink_open`).
- **Attributes:** `chmod`, `chown` to oneself, `utimensat` with nanoseconds,
  `touch`, `stat` fields, `ctime` updates, directory `mtime`/`ctime` updates on
  child changes, unique `st_ino` (`attr_*`, `dir_times_*`, `mtime_*`).
- **Permissions:** owner, group and other bits on files and directories,
  search and list bits, sticky, setuid/setgid bits stored, setgid directory
  inheritance, `access` (`perm_*`). These cases SKIP when the suite runs as
  root, because root bypasses the checks; the recorded runs were not root.
- **Errors:** the expected `errno` for each impossible operation
  (`err_*`, `trailing_slash_on_file`, `creat_existing_dir_eisdir`).
- **Locks:** `flock` (shared, exclusive, release on close, second descriptor)
  and `fcntl` byte-range locks, also across two clients (`flock_*`,
  `fcntl_*`, `peer_flock_exclusive`, `peer_fcntl_range_*`).
- **Sync:** `fsync`, `fdatasync`, `fsync` on a directory; a reopen after
  `fsync` sees the bytes (`fsync_*`, `fdatasync_write`), with every member
  up (P15).
- **`O_DIRECT`:** aligned reads and writes; 4 KiB read-modify-write inside a
  chunk; unaligned `O_DIRECT` fails `EINVAL`, as on a local file system
  (`direct_*`).
- **Extended attributes:** the `user.` namespace: set, get, list, remove;
  names up to 255 bytes, values up to 4,096 bytes, at most 16 KiB of
  encoded attributes per inode (`opt_xattr`).
- **`SEEK_HOLE`/`SEEK_DATA`** (`opt_seek_hole_data`).
- **`copy_file_range`:** the client has no handler, so the kernel copies through
  ordinary reads and writes; passes on the recorded hosts (`opt_copy_file_range`).
- **`statfs`:** sane values (`statfs_sane`); see `df` below.
- **Private mmap:** `MAP_PRIVATE` mappings work and never dirty the file
  (`mmap_private_not_visible`).
- **Concurrency on one mount:** creates, appends (one and two processes),
  disjoint and overlapping writes, create/unlink, readdir under writes
  (`concurrent_*`, `create_excl_two_proc`).
- **Two clients:** names, contents, sizes, attributes, links and removals made
  on one mount are seen on the other without a remount; races on `O_EXCL`,
  rename, unlink and hard links end with one winner and consistent state;
  overlapping and extending writes from both sides give the bytes POSIX
  requires (all 68 `peer_*` cases).
- **Durability across an orderly remount:** closed files with or without
  `fsync`, multi-chunk and sparse files, truncates, renames, unlinks, hard
  links, symlinks, modes, setgid inheritance, sizes and `mtime` (all 27
  `posix_persist.py` cases). This is an orderly unmount, not a crash.
- **`.efs` commands** (extras: `virt_stats_readable`, `virt_find_query`,
  `virt_find_not_a_real_dir`), while the cluster commands-policy allows them
  (the default; `EFS_COMMANDS` is removed, D42).

## Known deviations

| # | Deviation | What the user sees | Suite case | Disposition |
| --- | --- | --- | --- | --- |
| P1 | Shared mmap. Every open is `FOPEN_DIRECT_IO` and the client does not ask the kernel for direct-I/O mmap | `mmap` with `MAP_SHARED` fails (the kernel refuses it); private mappings work | `mmap_write_read` SKIP (extras) | Limitation, W104 |
| P2 | Read after `fork()` on kernels without the FUSE direct-I/O page-pinning fix (upstream `738adade96b2`) | A read in flight while another thread of the process forks can return success with the caller's buffer still holding its previous bytes; a retry reads correctly | Not in the POSIX suites; `tests/live/orcd_raft_kv/fork_repro.py` | Limitation K10/W121 (D40) |
| P3 | `renameat2` flags. The client refuses every flag | `RENAME_EXCHANGE` and `RENAME_WHITEOUT` fail `EOPNOTSUPP`, files unchanged. `RENAME_NOREPLACE` onto an existing name fails `EEXIST` (the kernel checks first); onto a name that does not exist it fails `EOPNOTSUPP` | `dir_rename_exchange_preserves_both_files`, `dir_rename_noreplace`, `dir_rename_noreplace_free_name` (finding F13) | Limitation (proposed) |
| P4 | `fallocate` | Mode 0 extends the size with a hole and reserves no space (`ENOSPC` can come later, at write); `FALLOC_FL_KEEP_SIZE` inside the file succeeds and does nothing; past the end, and every other mode (punch hole, zero range, collapse, insert) fail `EOPNOTSUPP` | `opt_fallocate` | Limitation (proposed) |
| P5 | Extended attributes outside `user.` | `security.*`, `system.*` (POSIX ACLs) and `trusted.*` fail `EOPNOTSUPP`: no ACLs and no SELinux labels on EFS files | `opt_xattr` covers `user.` only | Limitation (proposed) |
| P6 | `mknod` | Only regular files; FIFOs, sockets and device nodes fail `EPERM` and leave no name | `mknod_special_files_refused` (finding F13) | Limitation (proposed) |
| P7 | No client page cache. Every open bypasses the kernel page cache | Correct cross-client reads, but no cached rereads; `O_DIRECT` alignment rules still apply to applications that request it | Design (client Part D); covered indirectly by the two-client suite | Documented behavior |
| P8 | `df` reports logical 2+1 capacity | Total and free come from the 3-of-N bound over node quotas, not physical bytes; freed space appears after asynchronous unlink cleanup | `statfs_sane` | Documented behavior |
| P9 | `st_blocks` | Counts chunks present on the server per lane (D17), so `du` can differ from size/512 for files another client wrote | `sparse_st_blocks` | Documented behavior |
| P10 | Unpublished buffered writes | Writes not yet closed or synced are lost if the client process dies (D28); an orderly stop drains them or refuses | `posix_persist.py` covers orderly remounts only | Limitation (D28) |
| P11 | Large namespace operations | An operation touching more than 144 metadata participants is refused at once and atomically (D32) | Not in the POSIX suites | Limitation (D32) |
| P12 | Reserved `.efs` name | With `.efs` on, every directory resolves `.efs` to the command directory unless a real `.efs` entry exists, which wins; the command directory is not listed and not writable | `virt_find_not_a_real_dir` (not listed, not writable); the precedence of a real `.efs` has no POSIX case | Documented behavior |
| P13 | W159 | On a private four-member fixture on one host, the closing core run failed `dir_times_mkdir_rmdir` with `rmdir` `EBUSY` (orcd-mini) or timed out `dir_many_files` and `concurrent_creates_same_dir` (Intel); the same builds passed on the clusters. Diagnosis in progress (Oct 25): the fixture's members share one slow disk, and the metadata path's per-member `fsync` cost pushes apply waits past a fixed 400 ms budget, so RPCs take seconds; a fix is on a branch, not merged | `dir_times_mkdir_rmdir`, `dir_many_files`, `concurrent_creates_same_dir` | OPEN |
| P14 | Concurrent `O_APPEND` record loss (K9/W87) was never reproduced and has no root cause | No known symptom; the item reopens if a loss is seen | `concurrent_appends`, `peer_concurrent_append` PASS | Closed without resolution (D41) |
| P15 | `fsync` with a member down is degraded | `fsync`/`fdatasync` succeed when two fragments of every stripe the file's writes touched are proven durable and a third that is not is marked degraded; `EIO` with two of a stripe's nodes down or unproven. A member back from an unclean stop proves nothing until its verified recovery (K36) is done, which on a small store takes seconds | `tests/test_data_commit.c`, `tests/test_k35_fsync_degraded.py` (K35), `tests/test_k36_recovery.py` (K36); not in the POSIX suites | Limitation, *proposed* (findings F22, F23) |

Not POSIX deviations, but limits a user meets: no expiry options in the
`.efs` commands (expiry is operated with `efs-mgmt`, off by default; with
it on, a file whose deadline passed disappears from its directory like an
unlink and cannot get a second hard link while it has a deadline), no NFS
re-export, no macOS client, one export per cluster.
