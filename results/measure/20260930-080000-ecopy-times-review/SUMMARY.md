# SUMMARY — results/measure/20260930-080000-ecopy-times-review

ecopy --verify metadata mismatches (149: 119 atime, 29 mtime, 1 size) root-caused
to four client bugs — all fixed client-side, no wire or server change.

## Purpose and linked item

- Follow-up of `20260930-040600-postfix-review` /
  `20260930-044100-mtime-utimens-close`: the remaining mismatch classes
  analyzed with server-truth cross-checks (stat from a second client).

## Verdict

**FAIL** (four client defects demonstrated, then fixed): (1) atime/ctime
nanoseconds dropped by the staged table (`struct efs_inode_mem` lacked
atime_nsec/ctime_nsec; `inode_copy_attr`/`inode_to_rpc_p` dropped them);
(2)–(4) further client-side classes detailed in SUMMARY.txt. Server truth
verified correct for each.

## Source commit and build identity

Client tree `85f5b31c` (06:16Z build; 05:06Z mtime fix present, evictor fix
absent).

## Environment

User's fstor007 session 07:17–07:21Z; `ecopy.out.txt` (149 mismatches),
`ecopy.strace.txt` (305 MB), perf captures of efs-fuse during the copy.

## Method

ecopy --verify software/ with perf record + strace; mismatch classes split by
field; each class verified against a second client's stat.

## Results

| class | count |
|---|---|
| atime | 119 |
| mtime | 29 |
| size | 1 |

## Limitations and remaining gates

All four fixed client-side after this run; regression coverage in the posix
attr_* tests.

## Evidence index

- `SUMMARY.txt` — four-bug analysis (run `shasum -a 256 SUMMARY.txt |
  cut -c1-16` here)
- `ecopy.out.txt`, `fuse.data`, `flat.txt`, `callers.txt`, `by_thread.txt`,
  `strace-summary.txt`
