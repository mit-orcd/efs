# Contract/evidence review — Oct 7, round 6

[Queue](../status/README.md) · [Decisions](../status/decisions.md)

Review started **15:50:43 UTC**, with a **16:05:43 UTC** checkpoint deadline.
Baseline HEAD was `3a1b4a5225c0fdbf6956c2132edf8b2b2d5d197b` plus existing
working changes, including staged PUT-ticket metadata. Source and documentation
changed concurrently during this round; current queue statements distinguish
committed repairs, staged changes, reported tests and independently inspected
evidence. No production source fixes, commits, SSH, installations or cluster
mutations were performed. The documentation generator's appendix title was
updated for D31; generated artifacts are regenerated from source.

## Current status reconciliation

- W61/W62/W63 repairs are committed in `3a1b4a52`, rather than merely visible
  in the working tree. The [GC checkpoint](../status/gc-implementation-20261007.md)
  reports isolated direct-store tests: 216 passing single-client cases with
  one mmap skip, 64 peer cases passing, zero inventory/quota after restart and
  fenced late PUTs. Remote raw fixtures were **not retrieved in this round**;
  these are attributed checkpoint results, not independently repeated gates.
- Existing NUC services and xorinox incident stores are not established as
  rolled/reconciled by that checkpoint. Previously lost references cannot be
  recovered by a future sweep alone. The original 40 GiB incident stays open.
- [W86](../backlog/work-items.md#w86) indexes staged durable PUT tickets.
  Header/module/tests and atomic publication hooks exist. Production mount
  sessions, wire encoding, distributed authority and data-plane collection are
  still missing. [D31](../status/decisions.md#d31--recorded-abandoned-upload-policy)
  assigns a register identity to the narrow approval recorded in the checkpoint;
  this review did not independently retrieve the approving conversation and
  creates no new approval. D28's broader salvage choice remains unresolved.
- [W87](../backlog/work-items.md#w87) preserves the reported missing-record
  failure in both broad buffered concurrent-append cases. Ten isolated passes
  do not clear it. The traced full rerun needs a retained outcome; “in progress”
  is now explicitly a checkpoint statement, not live job status. W64 failover
  replay is a related gate, not a demonstrated cause of this failure.
- During this round a W85 working repair appeared in `write.c`: NEW ownership
  moves to the actual PUT attempt, retry cache misses probe, and placement-cache
  access is locked. A test file exists. No claim is made here that its author
  completed tests or server/quota acceptance; the old isolated reproduction
  remains an explicitly historical baseline.

## Earlier contracts and evidence

W23/P2.3 had contradictory current text: the index cited a completed run while
its detailed row said only a setup failure existed. Both now cite the
[completed Oct 5 summary](../../results/measure/20261005-040810-w23-stalled-compactor/SUMMARY.txt).
It records 4352 MiB/27 seconds and an RSS-threshold stop, but its zero-L0/no-effect
interpretation is **withdrawn**. Later inspection of raw samples and server logs
found [W89](../backlog/work-items.md#w89): four KV values occupy one TSV field,
so the derived peaks read shifted columns. The retained
[corrected expansion](round6-w23-samples-corrected.tsv) and
[peaks](round6-w23-peaks-corrected.txt) show node3 reached 22 L0 files /
3,367,253 bytes during the park (raw logs confirm), versus node1's 4 / 105,281.
The sampled RSS peaks are 30,316 / 25,936 / 25,624 KiB; sampled lag remains zero.
This is real L0 accumulation, not proof of the promised cap/lag bound. Original
result files were preserved untouched. Repair serialization and field validation
before the next appropriately preconditioned pressure run.

Source tracing through `kv_maybe_flush_locked`, `lsm_flush` and
`host_pub_batch_propose` distinguishes deferred flushing, optional waiting
flush callers and local legacy REPORT admission. In production apply with a
started compactor, cap/file-slot pressure kicks compaction and leaves the
memtable; it does not wait for the compactor there. The backpressure log line
alone therefore does not prove a pump stall. Local REPORT admission is not
universal proposal admission or cluster-wide follower-lag control. The 512 MiB
snapshot trigger remains distinct from a memory bound.

D8's register now records the historical ~3 ms answer and absence of an
approved larger-entry wire change, rather than implying its original measurement
question remains unanswered. D9 links the remaining W23 bound. D27's active
queue row and old measurement teardown advice now acknowledge implemented
runtime foundations without closing strict fault/timing/RSS gates. D17's queue
wording no longer generalizes its recorded du example as size/512 for every
file: source counts present chunks, takes the maximum with local pending
state, and handles the partial tail separately. The user guide already
distinguishes this from size-derived or physical root allocation.

The quorum-loss example now makes the commitment chronology explicit: Y is an
old uncommitted entry, later committed with the current-term Z; a higher-term
uncommitted X can survive on an isolated earlier leader. A four-voter sequence
is supplied. This preserves the no-per-index-merge conclusion while avoiding
an implication that a later leader can overwrite an entry already committed
in an earlier term. It is based on
[Raft §5.4.2, Figure 8](https://raft.github.io/raft.pdf), consulted Oct 7; no new
EFS consensus policy is introduced.

The [earlier W23/quorum explanation](contract-wording-before-round6-20261007.md)
is retained intact. Accepted decision contracts and raw existing results were
not removed.

## W88 isolated metadata reproduction

[W88](../backlog/work-items.md#w88) indexes D1's bounded legacy replay history.
The [fixture](round6-span-replay-repro.c) uses actual metadata apply against
in-memory KV, without sockets, FUSE or fragment stores:

1. Publish eight one-byte spans, retaining the first byte-identical request.
2. Fold them, then commit a later full-image mapping with another generation.
3. Publish a disjoint new span, which evicts the oldest folded tombstone.
4. Retry the original first span. It returns OK and becomes a live delta over
   the later full-image base.

[Output](round6-span-replay-output.txt): old generation 209, later base 242,
retry status 0, one reintroduced live old span. **Exit 1 is the expected failing
safety assertion**, not a passing release gate. The result was repeated against
a frozen source/header snapshot. Its
[dependency fingerprints](round6-fixture-dependencies.json) include the staged
metadata sources compiled in the fixture; the tested request uses the legacy
non-ticketed/non-durable-result path.

Example build from a matching repository snapshot on macOS:

```sh
cc -std=gnu11 -O1 -ffunction-sections -fdata-sections -Iinclude -Ideps/blake3 \
  -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 \
  -DBLAKE3_USE_NEON=0 -Wl,-dead_strip docs/archive/round6-span-replay-repro.c \
  src/kv/kv_mem.c src/kv/kv_key.c src/meta/meta_apply.c src/meta/metadata.c \
  src/meta/txn.c src/meta/session.c src/meta/put_ticket.c src/meta/dir_layout.c \
  src/meta/dir_spread.c src/sim/opid.c src/common/publication.c \
  src/common/checksum.c src/common/common.c deps/blake3/blake3.c \
  deps/blake3/blake3_portable.c deps/blake3/blake3_dispatch.c \
  -o /tmp/span-replay-repro
/tmp/span-replay-repro
```

For an ELF toolchain use its section-GC linker option instead of Darwin's
`-dead_strip`. Actual host/FUSE acceptance, exact bytes, physical old-object
availability and lost-ACK/restart tests remain owed. `host_pub_pack` permits a
first span with base_gen zero after `chunk_holds` loses its history; legacy
packing carries no durable publication receipt/floor. Staged receipt/retirement
primitives do not prove that production REPORT activates them.

## Final concurrent checkpoint

Before the deadline W85's repair was committed as `6d6056c3`. The GC
checkpoint now reports collision/concurrency helper and two-root quota/deletion
tests; this round did not independently rerun or retrieve those results. Active
status pages were reconciled to the commit while preserving earlier observations.
Ticket metadata evidence also expanded during the round; it remains staged,
and distributed/member-identity/wire activation gates remain open.

## Documentation checks

All five architecture gates passed after regeneration. Expanded links passed
across 37 active/root/review pages; W61–W89 each have one canonical queue row
and detail anchor, D31 has one register entry, all 30 corrected sample rows
have twelve fields, and `git diff --check` passed. No full runtime suite or
new live-cluster run was performed.

## Remaining review estimate

Approximately **5% ±5 percentage points** of documentation reconciliation
remains, mostly deeper legacy contract edge cases and independent retrieval of
new remote acceptance evidence. This is a subjective document-coverage estimate,
not proof of code correctness or completion of W fixes. Concurrent work requires
future status reconciliation even after this review ends. All unresolved gates
remain indexed, and acceptance needs its own tests and build evidence.
