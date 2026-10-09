# Consolidated devel acceptance — October 9 UTC

Production/test commit: 7b4f6085. Later 677522ec merges agent3's docs/evidence
only; src/include/tests/Makefile are identical to the passing build.

Merged outstanding committed work from devel-known-bugs, devel-gate-closure,
devel-compaction-pressure and devel-w66. Other development branches were already
ancestors. Branches retained. No push, tag or main merge.

| AMD mode | Core POSIX | Two-client | Persistence verify |
|---|---:|---:|---:|
| direct/TCP | 212/212 | 64/64 | 26/26 |
| buffered/TCP | 212/212 | 64/64 | 26/26 |

No failures/skips/not-run in these suites. Full Linux make all + make test
passed on final code; architecture gate 5/5 after final documentation merge.
First combined build 80ba3b89 also passed all six live suites before two newer
commits were incorporated and final gates repeated.

Hardware: AMD single host, four daemons on one NVMe filesystem; no distributed
failure-domain or hardware power-loss proof. TCP only; RDMA not exercised.
Core POSIX excludes optional extras per current suite design. This is AMD
acceptance, not a rerun of earlier failed Mac VM gates or W66 scaling closure.
D25 remains staged. Both command feature masks disabled.

Runner uses flock reservation, clean drains and original source/binary backup.
Original AMD installation restored direct/TCP after each cycle. Final health
records matching installed/backup daemon SHA256 and both disabled masks.
During retry, our restored daemons inherited fd9, retaining the reservation;
identity/start-time checks proved ownership, clean stop released it, and final
runner closes fd9 for child services. Final reservation is released with no
lock holder. No forced discard or reboot.

Shared /Users/mike/git/efs dirty documentation/index was preserved untouched
on preserved-doc-work-20261009 at original commit 2b9da49f. Clean devel checkout
is /tmp/efs-integration-20261009. Do not discard or blindly reapply those edits.
