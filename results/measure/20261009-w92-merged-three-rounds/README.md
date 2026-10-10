# W92 merged-source suites and three further profile rounds

Baseline: `8aeb9c4b`, `devel-find-stats`, Oct 9 EDT / Oct 10 UTC.
Xorinox files / direct I/O / TCP; user explicitly grants exclusive access.
No wipe, push, shared-devel edits or durability relaxation.

Merged-source gates: native `make test` exit 0; all five docs gates;
POSIX harness self-tests; 212/212 local baseline and EFS core, equivalent;
64/64 two-client; extras 4 pass / 0 fail / 1 unsupported shared writable mmap;
persistence 26/26 prepare and 26/26 after full daemon restart/fresh FUSE mounts.
NFS supplementary evidence: macOS `cp -X` 20/20 warm exact copies; after cold
daemon restart, independent fresh gateway reads verify all bytes and inode IDs.
This is not plain-cp or a cold macOS NFS client-remount gate: sudo needs a password.
Native FUSE and these narrower NFS checks do not close W55.

Each profile round uses the original tiny_files.py, four workers, 6000 files,
1..512-byte payloads, up to three directory levels. Namespace retained/growing;
60-second profiles can include idle tail when creation finishes earlier. No
build or frozen benchmark overlaps the CPU capture. Live throughput is observational.
Full scoped exported path sets and byte/directory totals plus 66 sampled SHA256
payloads per new tree are checked independently. Raw perf data stays remote at
/home/admin/w92-merged-three-rounds; reports and integrity markers are retained here.

Controlled syscall comparisons use the same frozen 25425-file / 827-directory /
6234928-byte namespace and merged supporting library, one fresh cache build and
three unchanged refreshes. Traced time is not a performance measure. Alternating
untraced repetitions provide CPU/wall ranges; filesystem caches are warm/shared.

The first focused test run had an incorrect syscall expectation (ignored the
EOF probe in each short index/raw file); the corrected expectation and all
focused/differential/quota gates pass. Both original and retry logs are retained.

Round 2's driver was edited while bash was reading it, causing a trailing parse
error after all three profiles completed. The isolated oracle was then run
separately and passed. The driver is syntax checked and left unchanged during
round 3. Profile exit markers are independently checked for all nine captures.

Final runtime `a8a15a14` is verified on all storage nodes (wire build ID; the
human version string uses the no-git-metadata fallback in this private copy).
Matching source hashes, direct-I/O arguments and TCP environment are logged.
All eight retained trees cold-check 60000 exported paths/totals and 526 sampled
payload hashes. Root counts: 62100 files, 2174 directories, 177570052 bytes;
refresh error 0. Diagnostic mount removed; gateway and two ordinary clients
mounted with `.efs` disabled. No retained EFS data was deleted.

The native pass used the f4f95ed7 build stamp: runtime/test source is identical to
8aeb9c4b (the latter adds documentation/evidence). Subsequent cache-only changes
have focused/differential/quota acceptance; a second full native pass is not claimed.

Controlled four-build syscall totals are recorded for r0/r1/r2/r3. Preads fall
1295903 → 285379 (78.0%), pwrites 381549 → 291549 (23.6%), fsync stays 5363.
Accounted builder memory grows 2103112 → 2897032 bytes, below 4 MiB. Three
alternating untraced repetitions each show CPU median 2.240916 → 1.588996 s
(29.1% lower); refresh median .900269 → .741644 s with overlapping ranges.
See summary.json for all ranges, validation.json for integrity and cold checks,
and the indexed status note for remaining W92 obligations. Live creation is
131/128/127 files/s with a growing retained namespace; no foreground gain claimed.
