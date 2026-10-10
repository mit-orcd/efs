# W92 — tiny-file disk-catalog hot paths

Oct 9, 2026; `devel-find-stats`, deployed at this checkpoint: `6e841662`.
The [three additional rounds](query-cache-three-rounds-20261009.md) supersede
this deployment and retain these measurements as historical evidence. Three Xorinox VMs,
files/direct-I/O/TCP; ordinary mounts opt out of commands.

The user's tiny_files.py ran on xefs1 against EFS: two 12000-file trees, four
workers, 1..512-byte payloads, up to three directory levels. All-node perf found
repeated tiny preads through binary-search metadata lookup dominating catalog
CPU. Added fixed bounded read windows and avoided filename formatting on hits;
write-through coherence and partial-write error invalidation are regression-tested.
A subsequent profile identified cache fsync costs: immutable unchanged hard-linked
raw/index/name files now reuse their prior durable digests. Changed/new files and
publication barriers remain persisted and checkpoint restore still verifies them.

Against identical stopped metadata copies (25425 files), median unchanged cache
refresh fell from 1.977 s to 0.955 s (three repeats); initial build from 3.933 s
to 3.226 s. The deterministic 12000-file differential harness's pread count fell
50.2%, with pwrite count unchanged. Accounted candidate memory increases by a
fixed 262656 bytes; this is not an RSS ceiling or an incremental-stat implementation.

**Live creation drop remains unisolated:** 225 to 85 files/s across the two live
runs. The latter retains a larger namespace and includes other merged devel
changes; this is not a cache-only creation A/B. W66/W92 follow-up must control
revision, namespace and placement before assigning cause or claiming speedup.
Do not relax persistence or metadata authority to regain throughput.

All paths and counts/bytes of both trees match independent filesystem models;
65 sampled payloads per tree match after the final restart/fresh mount. RAM/disk,
restore/refusal, quota and partial-ENOSPC/window regressions plus related catalog,
name-index and metadata gates pass. Full suite and sanitizer acceptance are not
claimed. Daemons and ordinary mounts restored, diagnostic mount drained/removed;
no wipe or push. See [raw ledger and scope](../../results/measure/20261009-w92-tiny-profile/README.md).

W92 remains open for full incremental derived maintenance, aggregate disk-space
control and large-scale/fault/platform acceptance. [Status index](README.md) ·
[Work item](../backlog/work-items.md#w92).
