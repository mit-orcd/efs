# SUMMARY — results/measure/20261008-amd-link-symlink

Link/symlink syscall-semantics probe: link(2)/linkat(2) follow-vs-nofollow
behavior on local fs vs the efs mount, AMD host.

## Purpose and linked item

- Hardlink/symlink semantics verification (linkat AT_SYMLINK_FOLLOW handling)
  on the AMD cluster mount.

## Verdict

**PASS** (probe completed): on `/home/efs/efs/mnt` the nofollow case yields a
symlink (is_symlink=true, source_ino==target_ino as expected), follow resolves
to the target; `linkat(..., AT_SYMLINK_FOLLOW)`, plain `linkat`, and `link`
all exit 0 in the strace (`amd-link-symlink-syscalls.txt`, exits with 0).

## Source commit and build identity

Not recorded in the run.

## Environment

AMD host, efs mount at /home/efs/efs/mnt; local /tmp as reference.

## Method

`amd-link-symlink-probe.jsonl` per-case records (default/nofollow/follow/
libc_link on both roots) + strace `amd-link-symlink-syscalls.txt`.

## Results

| case | /tmp | efs mnt |
|---|---|---|
| default | not symlink | not symlink |
| nofollow | symlink (ino unchanged) | symlink (ino unchanged) |
| follow | target ino | target ino |
| libc link | rc=0 | rc=0 |

## Limitations and remaining gates

Probe scope only; regression coverage lives in the posix symlink/hardlink
tests.

## Evidence index

- `amd-link-symlink-probe.jsonl`, `amd-link-symlink-syscalls.txt` (run
  `shasum -a 256 <file> | cut -c1-16` here)
