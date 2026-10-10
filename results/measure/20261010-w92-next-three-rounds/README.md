# W92 integration and further three-round evidence — October 10, 2026

Isolated devel-find-stats, exclusive Xorinox VMs, files/direct-I/O/TCP.
Branch boundary and ancestor proof are retained; no other worktree is modified.

Three 60-second 99 Hz profiles on all three storage daemons. Original tiny_files.py
creates 6000 files per round, four workers, payloads 1..512 bytes, up to three
folder levels. Random placement and retained namespace growth make live throughput
observational. CPU captures can include idle tail; no compiler or frozen benchmark
runs during capture. Remote perf binaries remain in /home/admin/w92-next-three-rounds.
Decoded reports, exit markers and checksums are retained locally.

Frozen comparisons use /home/efs/w92-frozen-after: 25425 files, 827 directories,
6234928 logical bytes. Each process performs a fresh build and three unchanged
refreshes, with a fresh private cache, 4 MiB builder budget and 64 MiB snapshot
budget. Supporting merged library is the same; query_disk.c variants are explicitly
linked ahead of libefs.a. Variants: r0 merged baseline 51880999, r1 parent folding
c37b0519, r2 descriptor index f5164bb3. Traced elapsed time is not a speed measure.
repeat_frozen.py alternates three untraced trials per variant and preserves every
observation, recording process CPU/RSS with wait4. Shared filesystem cache is warm;
these are VM observations, not cold physical-device acceptance.

Merged native make test initially fails solely because the private build copy omitted
linked result evidence. Actual referenced artifacts are copied, then the complete
suite passes. Original and retry logs are retained. Each cache fix also passes the
actual-source IO/fault test, RAM/disk differential test at default and 12000 files,
bounded-memory admission and generation quota refusal. No persistence barriers are
removed; failed candidates preserve the old checkpoint.

Cold validation retains independent full path/count/byte/directory models and sampled
SHA256 payloads, external to EFS. Scripts are stable while running. No raw perf binaries
or compiled executables are committed here. The indexed status note contains outcomes,
remaining work and deployment details.

All nine captures exit 0 and report zero lost samples. Profile integrity collection
initially lacked permission to hash root-owned captures, then hit local shell quoting
of a remote PID; sudo/heredoc collection succeeds. Original diagnostic logs remain.
These collection errors are not workload failures. Final native make test passes.

Controlled totals: preads 285379 -> 173923 (39.1% fewer), pwrites 291549 -> 189853
(34.9% fewer), fsync unchanged at 5363. Accounted memory 2897032 -> 2914016 bytes.
Three alternating trials each show median process CPU 1.578488 -> 1.341862 seconds
(15.0% lower); descriptor indexing contributes 3.3% beyond parent folding. Refresh
ranges overlap, including one slower final observation. Live rates 133/133/131 files/s
do not establish a foreground speedup. Full repetitions and ranges are in summary.json.

The final cold-restart driver drains every client and stops all three daemons, then
its redundant inter-node binary stream stalls in stale cluster SSH multiplexers.
Only task-owned stalled SSH children are cancelled. Fresh SSH confirms all daemons
inactive and all three installed efsd SHA256 values identical (f5164bb3). Closing
only cluster-script mux sockets lets resume_cold.sh start those verified binaries.
Original stalled-transfer/driver logs and the resumed startup are retained. This
operational interruption does not invalidate the completed native/live gates or
CPU comparisons and is not evidence of an EFS daemon shutdown failure.

Final persistence passes 26/26 before and after full daemon restart and fresh FUSE.
Core POSIX is 212/212 equivalent to baseline; two-client POSIX 64/64; extras 4 pass,
0 fail, 1 unsupported shared writable mmap skip. All eleven retained tiny trees
cold-check 78000 files and 724 sampled SHA256 payloads. Root: 80756 files, 2927 dirs,
344207151 logical bytes, complete=true, refresh_error=0, refresh_overdue=false.
The diagnostic mount is removed; all ordinary mounts retain .efs opt-out. Final
source/binary provenance and integrity are in node*-final-runtime.log. No EFS
namespace is deleted. Native FUSE tests do not close NFS-specific W55.
