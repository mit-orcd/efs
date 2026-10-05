# Project status — the task right now and the work queue

[Architecture](../how-it-works/architecture.md) ·
[Backlog](../backlog/README.md) · [In flight](in-flight.md) ·
[Decisions](decisions.md) · [Operations](../operations/operations.md)

This page exists because of the bar in [developing.md](../how-it-works/developing.md): **a
less advanced model must be able to contribute a correct change.** That is
only true if finding *the next task* and *the exact pages that govern it* is
mechanical. Read this page, then read at most the two or three files it
sends you to — not the whole spec.

---
## 1. The task right now

**Where the project is (Oct 2 2026).** [architecture.md §10](../how-it-works/architecture.md)
steps 0–12 are landed and gated: simulator, KV, Raft, cross-shard txns,
sessions, directory spread, delete-2PC, FUSE A–D. The Raft+KV engine is
the only metadata engine (Step 11, Sep 11). There is no next §10 step.
What remains is the work queue in [§1a](#1a-the-work-queue): measured
gaps, in order. The live cluster state (build, leaders, recorders) is the
cluster fact in `.cursor/rules/efs-project-state.mdc`, not this page.

**How to pick work.** Finish [§1b](in-flight.md)
first. Then take the lowest-numbered open item in §1a; correctness before
performance. Each item names what to change, how to measure it, what
proves it, and what is forbidden. If an item needs a decision the spec
does not contain, **stop and ask** (§4). Decisions already taken are the
rows marked **decided** in §1a "Decisions"; implement them, do not
re-ask. **Oct 2 19:05Z: two open correctness rows go first — W54 (row
0i, a fold's GC deletes the live base → read EIO, data loss) and row 0j
(the client's 50 ms lookup memo returns pre-mutation stats, posix
164/201).** Open asks as of Oct 2 13:45Z: **D28** (who owns acknowledged
bytes across client death), **D29** (a REPORT receipt for "committed,
apply pending"; its live symptom did not appear untraced — P0.2),
**D30** (the remedy for compaction-induced apply lag), **W53**
(keep or revert W41 given the create/close-storm p99 37 → 100–120 ms
under four concurrent 8 GiB writers), **W52**'s fix shape (serial
reservation resolves make an O_APPEND REPORT exceed 30 s). Open
**investigations** — evidence gathering, not approved implementations:
**W48** (four dd streams ended early), **W53**. **W50** and **W51**
closed as investigations (14:57Z / 14:38Z). D27 was
corrected 04:20Z and is to be implemented as its row now reads. The
performance plan's P0, P1, P2.1 and P2.4 are done (§1b); **P2.2 D26**
is in tree and gated on the dev cluster (Oct 4: per-anchor pending-GC
watermark + the cursor; an idle leader runs no frag scan at all).
Everything else in the Oct 2 plan is decided, asked, closed or
deferred — see that table.

**What this page is not.** Approvals and decisions recorded here are
document claims about what the user said on the date given; they are
not authorization to mutate the cluster now. The long-form item texts
(sources, steps, forbidden lists for W6–W25) are in
[work-items.md](../backlog/work-items.md); superseded handoffs, closed items and
the old ordering tables are in project-history.md — cite its anchors:
[handoff archive Oct 1 18:00–22:00Z](../archive/project-history.md#ph-start-here-handoff-archive-oct-1-2026-1800z--2200z--moved-oc-8297ba),
[handoff archive Sep 28 – Oct 1](../archive/project-history.md#ph-start-here-handoff-archive-sep-28--oct-1-2026-1400z--moved-o-4e37f3),
[closed items moved Oct 2](../archive/project-history.md#ph-start-here-closed-items--moved-oct-2-2026-ea8027),
[closed items moved Oct 1](../archive/project-history.md#ph-start-here-closed-items--full-text-w1w5-w7-w11-w13-moved-oct-7d6df9).

**Before touching the cluster** run `tests/preflight.sh` (the deploy
rule's pre-flight as one command). Stop/start is
`tests/cluster.sh stop|start|restart [--clients] [--perf] [--strace]`;
a fresh table after a wipe is `cluster.sh start --fresh`. Runbooks for
the open measurement items are in [runbooks.md](../operations/runbooks.md)
(`tests/measure/*.sh`). **The one live cluster is port 19810** on
fcstor003–006 (`/data1/01–06/efs`, `--quota 36T --direct-io`, RDMA),
clients fcstor003–015 at `/tmp/efs-mount`, the user's own client on
fstor007 via `scripts/client.sh`. 19820 is retired. Do not
`wipe_cluster.sh`, `pkill -x efsd`, or `raft-mkfs` without being asked.

### 1a. The work queue

Rules for this queue: **take the lowest-numbered open item.** Do not start a
later item to avoid a harder earlier one — the order encodes a dependency and
a severity: **correctness items come before performance items**, because a
parallel filesystem that returns wrong bytes has no throughput number worth
reporting. Do not batch two items into one change. Every item ends with
`Forbidden`, which is binding.

| # | item | class | status | home |
| --- | --- | --- | --- | --- |
| 0i | **W54** · a fold's GC deletes the live base → read EIO, data loss | correctness | open — do first per §1; fix in `meta_apply.c`, unit test, roll | [full text](../backlog/work-items.md#w54--a-folds-gc-deletes-the-live-base-queue-row-0i) |
| 0j | the client's 50 ms lookup memo returns pre-mutation stats (posix 164/201) | correctness | open — do first per §1; invalidate the memo on local mutation | [full text](../backlog/work-items.md#0j--the-clients-50-ms-lookup-memo-returns-pre-mutation-stats-queue-row-0j) |
| 0k | **W55** · span committed to raft, fragment PUTs never landed → read EIO, data loss | correctness | open — found Oct 5 on the xorinox test cluster (write via the nfsd re-export from a macOS client); trigger not isolated | [full text](../backlog/work-items.md#w55--span-committed-to-raft-fragment-puts-never-landed--read-eio-data-loss-queue-row-0k) |
| 0e | **W38** · ior-hard fold tombstone without the span's bytes | correctness | open — client-side fix; traced IOR-hard + cold `hardscan` gate | [full text](../backlog/work-items.md#w38--ior-hard-fold-tombstone-without-the-spans-bytes-queue-row-0e) |
| 0c | **W36** · rename-vs-unlink of one source both succeed, dangling dentry | correctness | in tree — gate owed: posix2 `peer_rename_vs_unlink_src` 20/20 | [full text](../backlog/work-items.md#w36--rename-vs-unlink-of-one-source-both-succeed-dangling-dentry-queue-row-0c) |
| 2a | **W42** · `df` / `efs-mgmt status` report the 3-node capacity model on any node count | correctness | in tree — verify on 19810; one-QUOTA-member PUT question open | [full text](../backlog/work-items.md#w42--df--efs-mgmt-status-report-the-3-node-capacity-model-on-any-node-count-queue-row-2a) |
| 0b | **W27** · REPORT identity from the staging table | correctness | rerun on the current client; close if `putid miss` is 0 | [full text](../backlog/work-items.md#w27--report-identity-from-the-staging-table-queue-row-0b) |
| 0g | **W43** · big truncate is a silent no-op past 32 chunks per lane; step a = **D25** (decided) | correctness | steps b/c in tree; D25 implementation next | [full text](../backlog/work-items.md#w43--truncateo_trunc-of-a-file-with--32-chunks-in-a-lane-is-a-silent-no-op-queue-row-0g) · [D25](decisions.md) |
| 0a | STALE replay that never converges; remedy = **D27** (decided) | correctness | implement D27 as its row reads, with 0a (a)–(c) | [full text](../backlog/work-items.md#0a--stale-replay-that-never-converges-queue-row-0a) · [D27](decisions.md) |
| 0h | **W44** · the leader's GC frag pass scans the whole prefix every 1.2 s; remedy = **D26** | performance | D26 in tree + gated (dev cluster, Oct 4): watermark gates the scan, cursor bounds the pass; idle leaders logged no `gc-pass` line for 10 min; a 5120-record `rm` drained at ~514 records/pass | [full text](../backlog/work-items.md#w44--the-group-leaders-gc-frag-pass-scans-the-whole-prefix-every-12-s-queue-row-0h) · [D26](decisions.md) |
| 12 | **W48** · four of the 16 dd streams ended early | investigation | investigate — evidence only | [full text](../backlog/work-items.md#w48--four-of-the-16-dd-streams-ended-early--investigate-plan-row-12) |
| 15 | **W52** · a REPORT after thousands of O_APPEND writes answers after > 30 s | correctness | decided Oct 5 (user): shape A — one batched proposal for all caught-up reservations before the reply; gate = `append_gate` | [full text](../backlog/work-items.md#w52--a-report-after-thousands-of-o_append-writes-answers-after--30-s-plan-row-15) |
| 16 | **W53** · W41's create/close-storm p99 regression — keep or revert | investigation | investigate — evidence only, then the user decides | [full text](../backlog/work-items.md#w53--w41s-createclose-storm-tail-under-concurrent-big-writers--investigate-plan-row-16) |
| E | **D17** · `st_blocks` = 0 for files this client did not write | performance | in tree + gated (dev cluster, Oct 4): lane-stamp present count, summed at getattr, client takes max with its local table; `du` on a non-writing client = size/512 | [D17](decisions.md) |
| P2.2 | **D26** · the GC pass | performance | in tree + gated (dev cluster, Oct 4): per-anchor pending-GC watermark maintained in the apply, derived once per recovery/import, `zero_if` clamp on a drained pass; `test_gc_watermark`; live: 514 records/pass drain, 10 idle min with no `gc-pass` line | [full text](../backlog/work-items.md#p22--d26--the-gc-pass-performance-plan-row) · [D26](decisions.md) |
| P2.3 | **W23** · stalled-compactor test | performance | test + hook in tree; measured Oct 5Z (dev cluster): no stall-specific effect to 4.35 GiB (n_l0 never left 0 — the stall never bit; rss-2x stop = small-VM calibration artifact); refinements named in SUMMARY | [full text](../backlog/work-items.md#p23--w23--the-stalled-compactor-test-performance-plan-row) · [run](../../results/measure/20261005-040810-w23-stalled-compactor/SUMMARY.txt) |
| P3 | `efsd --bench`, then `efs-fuse --bench` | performance | tools in tree + gated (dev cluster, Oct 5); first numbers measured on efs1 (write ≈82 % of the 1-disk fio ceiling at QD16; meta = the 36 ms fsync wall, batch ×32; client cpu 2.8 ≫ put 0.26 ≈ write GiB/s). Owed: the 6-NVMe fcstor run (named host) and the two-host client ladder | [server plan](../backlog/work-items.md#single-node-storage-bench-efsd---bench--asked-oct-2-2026-user-queue-position-after-w41--d23--d17--d26-in-plan-after-the-oct-1-2200z-review-its-number-decides-the-fragment-layout-w40-and-zero-copy-receive) · [client plan](../backlog/work-items.md#client-bench-efs-fuse---bench--asked-oct-2-2026-user-after-efsd---bench) · [run](../../results/measure/20261005-045140-p3-benches/SUMMARY.txt) |
| P4.1–P4.4 | fragment on-disk layout (**wipe**), W40 FUSE write copy, RDMA zero-copy receive, 9-client scaling | performance | deferred until P3's numbers | [full text](../backlog/work-items.md#p4--deferred-until-p3s-numbers-long-one-is-a-wipe) |
| — | **D28** (who owns acknowledged bytes across client death) · **D29** (a REPORT receipt for "committed, apply pending") · **D30** (the remedy for compaction-induced apply lag) | ask | ask — not code until the user decides | [decisions.md](decisions.md) |
| — | **io-stats** · always-on per-op-class data-plane counters (`iostats:` log line + `efs-mgmt io-stats`) | observability | landed (dev cluster, Oct 4) | [note](../backlog/io-stats.md) |
| — | **version reporting** · `--version` on all five binaries, startup log lines, `EFS_MSG_VERSION` op, `efs-mgmt version`, `status` Versions line | observability | landed (dev cluster, Oct 5) | [code](../../include/efs/version.h) |

**Order of work from here (one order, correctness first — Oct 2
04:20Z):** (1) **D25** (logical truncation + background reclamation) completes W43 (`truncate_big.sh` exit 3 → 0 with t1–t9, `apply_max` flat);
(2) **D27** with 0a (a)–(c) and its gates; (3) the correctness gates
still owed — W36 20/20, W38 (row 4), W27 rerun (row 6), the W42
one-QUOTA-member PUT question; (4) only then the measurements — W44 a
idle-hour reading, W46/W47 counts (rows 9/10), the untraced 16× dd
re-measurement (row 11); (5) D23, W41, D17, D26 (after step 4's W44
reading); (6) `efsd --bench`, then `efs-fuse --bench`; (7) the open
asks go to the user — D28 with 0a (a)'s answer and D27's gate result,
D29 with W41, D30 with W51's table. The investigations W48 / W50 / W51
(rows 12–14) are quick evidence tasks that may run at any point the
cluster is up; they are not approvals to implement anything. §1b
"Next" is this list and nothing else.

**Dependencies, in one line.** P0 needs a cluster roll (no `--strace`).
P1.1 and P1.2 are independent of each other and of the servers; P1.2
wants P1.3 decided to realise its full effect but does not need it.
P2.2 waits for P2.1; P2.5 waits for P2.4. P3 needs no cluster. P4 needs
P3. Nothing here needs a wipe except P4.1.

Hardware ceilings and honest baselines now live in [performance.md](../how-it-works/performance.md#baselines-and-ceilings-current), including the normative sentence "if a benchmark stops at a mutex … that is by definition an EFS bug" and today's "write path = 9 % of client ceiling" state.
