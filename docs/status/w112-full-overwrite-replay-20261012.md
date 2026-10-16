# W112 — acknowledged ordinary full-overwrite loss on STALE replay

Status: closed for the reproduced canonical full-overwrite STALE replay defect;
repair and full merged/live qualification pass at `02b9918b`. Cluster: AMD, isolated four-daemon TCP fixtures,
both direct and buffered storage. Production stores/binaries unchanged.

## Reproduction and byte evidence

Frozen `d9d39acc` passes full normal/fault native and ten sanitizer gates, yet
this live sequence fails: create/fsync a512-byte file; read it from a peer;
replace it with262,160 bytes (`ordinary-growth:` followed by256-byte sequences);
fsync and close successfully; open/read on the primary, warmed peer, fresh
third mount, then after cold daemon restart. All reads have the correct size
but wrong SHA256, differing from offset0. Both modes reproduce identically.

Actual bytes exactly equal the old512-byte payload, zeros through byte131,071,
then the new payload from131,072 onward. Truncated tiny-1 and untouched tiny-3
remain exact. This is acknowledged overwrite loss, beyond a warmed-cache issue.
[Raw two-mode byte audit](../../results/measure/20261012-w112-overwrite/audit/)
and [expected/actual/cold payloads plus cache trace](../../results/measure/20261012-w112-overwrite/trace/) are retained.

## Cause and bounded repair

The trace shows the new full chunk PUT succeeds; REPORT rejects its stale
observation of the old span. `dcache_replay_stale_ex_budgeted` then fetches the
old published chunk. For nrange0/have_base1/full UNCOND ownership, its old branch
refuses the immutable local full image when committed differs from the new
object and republishes the fetched old image, then acknowledges fsync.

The repair retains every accepted full-overwrite byte only for the canonical
full ownership marker (UNCOND base, complete local image, no ranges or object
span). It binds the retry to the freshly observed committed generation and
retains the observed span/fence metadata for publication CAS. Partial ranges,
cleared span state, peer bytes, failed fetches and ambiguous ownership retain
their existing refusal/merge paths. No publication, fence or durability barrier
is removed.

The actual production-function [old-source regression](../../results/measure/20261012-w112-overwrite/old-source-regression.log) fails at d9d39acc; the fixed
regression and existing peer-byte/tombstone/race/error tests pass normally and
with ASan/UBSan. A new `--ordinary-overwrite-only` live fixture covers former
512-byte spans becoming a full chunk, two chunks plus a tail, full zero bytes
and a partial replacement, immediate primary/peer reads, cold bytes and
physical cleanup. Its Linux both-mode qualification passes at `02b9918b`.

## Closure gates

Pass new ordinary-overwrite live gate in both modes, full merged normal/fault
native and sanitizer gates, and retain exact source/binary/configuration
identity. Review the canonical ownership boundary independently. Other owners' unfinished acceptance remain separate release
blockers even after this repair. No push, tag or promotion.

## Qualified closure checkpoint

At frozen `02b9918b`, complete AMD Linux normal and EFS_FAULTS=1 `make all test`
and eleven ASan/UBSan gates pass. All 1,335 source/linked-evidence inputs remain
unchanged. Both direct and buffered ordinary-overwrite runs pass every exact
primary, warmed-peer and cold SHA256 oracle for all four cases, then physical
fragment and metadata queue cleanup; normal mounts restore successfully.
[Full gates, binary identities and raw both-mode evidence](../../results/measure/20261012-v030-overwrite-qualified/02b9918b/acceptance-summary.json)
include 41 retrieved artifacts verified against their SHA256 manifest.

Independent [boundary review](../reviews/w112-overwrite-20261011/README.md)
reproduces the old-source failure and passes fixed generation/fence, partial
range and retained-span probes normally and under sanitizers. This closes the
reproduced ownership defect; it does not certify unrelated writeback paths. Direct seven-client/jobs16 still fails separately.
