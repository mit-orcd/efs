# K13 / W86, W88, W137: the ticketed PUT/REPORT path as the default

**Verdict: PASS (scoped) on branch `devel-k13` (`c860c3dbd`, merged with
devel in `a79b59de1`), not merged into devel.** The default PUT
and flush now run only through durable PUT tickets (W86), the W88 folded-span
retry no longer resurrects after trailer eviction in the production REPORT
path, and W137 tests the real reply shape. **INCOMPLETE:** the D31
all-member collection of abandoned uploads is not implemented (see
Limitations). **NOTRUN:** RDMA, orcd-mini, multi-host.

- **Purpose:** alpha 1.0 checklist row K13, decided FIX by
  [D40](decisions.md) (Oct 23): finish W86 and the W88 replay, keep W137
  required. Policy: [D31](decisions.md#d31--recorded-abandoned-upload-policy).
- **Tested source:** `c860c3dbd` on devel `bf16436e8` (one commit: code and
  tests). The build tree was `git archive c860c3dbd`, so the build ID reads
  `unknown`. sha256: `efsd` `846286e0…8a1e36`, `efs-fuse`
  `6ac8c638…3d8c4f`, `efs-mgmt` `b6b1c526…844e54`, `libefs.a`
  `10ca43b5…71530d` (full hashes in the capture list below).
- **Environment:** intel1, Rocky Linux 10.2, kernel 6.12.0-211, 8 CPU,
  15 GB, gcc 14.3.1, Python 3.12.15, fuse3 3.16.2. Work tree and stores under
  `$HOME/efs-k13` (XFS). The user's live cluster on 17432–17433 and its mount
  were not touched. Private fixture: three `efsd` on 127.0.0.1:18700–18702,
  `EFS_MD_RAFT_N=3`, `EFS_TRANSPORT=tcp`, `--no-direct-io` (buffered I/O),
  `--quota 4G`, one `efs-fuse` mount, 128 KiB chunks.

## Merge with devel `d2369a49d` (W154, K14, K30, W155, D39) — `a79b59de1`

The branch was brought up to devel by a merge (`a79b59de1`), reviewed rules
applied. Gates on `git archive a79b59de1`, intel1, same environment:
**`make test-all` exit 0, 0 lines matching `^make(\[[0-9]+\])?: .*Error`,
419 test commands, the same 15 NOTRUN lines; private ticket gate PASS**
(now with a W154 phase).

- **W154 kept on the ticketed PUT.** `put_fragments_parallel_once` still
  reports which fragments were acknowledged. A two-fragment success is logged,
  recorded in the bounded `dg_obj` table under the ticket's object and stamped
  as `EFS_CHUNK_REC_F_MISSING` on the REPORT record; a full table fails the
  write and gives the ticket up.
- **Flag bits.** `HOST_PUB_F_REJECT_GONE` is 128 (W154 owns 16/32/64), with
  a `_Static_assert` against the missing mask. The REPORT and publication
  validators accept a valid missing mask and still require the ticket.
- **K14 staged publication carries the ticket.** The stage record (version 2,
  238 bytes) carries the ticket and the W154 missing mask, which version 1
  dropped. RESOLVE COMMIT publishes the ticket in the batch that applies the
  mapping. The owner's settle is BUSY while a stage on that chunk is pending,
  so it never abandons an object a unit may still publish. Unit record
  buffers hold the ticket trailer.
- **W154 rebuild on the ticketed storage path.** A published object has no
  ADMITTED ticket, so a rebuilt fragment for a remote member goes in a new
  `EFS_MSG_REBUILD_PUT` (175/176). Storage stores it only when the mark's
  group, after a ReadIndex, holds exactly that degraded mark (RPC-only
  `EFS_MD_REQ_DEGRADED_CHECK`, tag 85).
- **KV kinds** `PUT_ORPHAN` 53 and `PUT_COUNT` 54 (51 is taken). 129/130 are
  listed as retired message IDs.
- **Gate:** a three-daemon W154 phase was added. n3 stopped, a 4 MiB write
  published degraded, n3 returned; all 32 missing fragments were stored on n3
  through `EFS_MSG_REBUILD_PUT`, and the file then read exactly with n1
  stopped. The leader-death check now treats a short read (a later page's
  EIO) as unreadable after verifying its prefix.
- **Captures:** `$HOME/efs-artifacts/k13-20261023/merge-a79b59de1/` on intel1:
  `maketest-all-a79b59de1.log`
  `1f114ac5f289e0357e50744abcce9b5242f1babe7a79479783ff5bf46b67c181`,
  `gate1.out` `0874c82075792450bc0e7066014c7f9f864908e8921fc4eb6aa2a4952f886ea9`,
  `gate1-logs.tar.gz`
  `8c65d8762c801c5457a073d3aa98cfb0d9c7f9fd154371d6c6a84fb75f5ddb52`.
  Binaries: `efsd` `6ad31542d82ef1d1167e613403c85089b8787f551517a27534003d6b4b45e58b`,
  `efs-fuse` `a992978cf3a53e0179380123da0cd2b7738af9035624632eca9479636b70db0e`,
  `efs-mgmt` `396a8aada67db92da64f5a91807f940d064f4695d34449ba021d73704a838e34`,
  `libefs.a` `b86e99849e6eb92a9778c60bf7f59e833e2ed8f1e1abd4dc91f60b46667e50af`.
- **Found in devel, not fixed here:** `EFS_KV_KIND_DEGRADED` (W154) and
  `EFS_KV_KIND_PUB_STAGE` (K14) both use kind 51.

## What changed

- **One data PUT.** Before its first fragment, every PUT object admits an
  immutable ticket: FileID, chunk, placement, checksums, span range,
  fragment length and member mask. The object name derives from the ticket
  sequence. `EFS_MSG_PUT_CHUNK` carries the 224-byte ticket header.
  Storage stores a fragment only for an ADMITTED ticket (fresh ReadIndex
  STATUS), inside the session storage fence. The unticketed PUT, the staged
  `PUT_TICKETED` opcode and its 13-byte reply are gone; the reply is the
  10-byte status/path/boot-verifier frame with a new terminal STALE status.
  Protocol version 5 refuses older peers.
- **Ticket-bound REPORT.** Every REPORT record carries its ticket. The host
  refuses an unticketed or never-admitted record and a bare `PUBLISH` log
  entry. PUBLISHED joins the mapping batch.
- **W88 fix.** Replay authority is the ticket, not the eight-entry span
  trailer. A PUBLISHED ticket is an exact no-op; a retired or settled one is
  STALE and never publishes again.
- **CAS loss and deletion.** A lost CAS keeps the ticket ADMITTED, so the
  same object can retry on a fresh base. A deleted file settles the ticket
  GONE.
- **Staged typed endpoint.** The host now refuses unticketed requests on the
  typed endpoint and in its log commands. The staged client driver is still
  not used by FUSE.
- **Ledger.** The ticket ledger gains a REJECTED state, an orphan ledger and
  a counted 4096-ticket bound per session/lane-shard stream. The owner's
  SETTLE command (batched, in stream order) retires a PUBLISHED ticket, or
  abandons an unreported one. An abandoned ticket's body is recorded for
  revocation-gated collection. Settling a never-applied admission records it
  REJECTED, so a late admission is refused.
- **Client.** The mount ledger admits each ticket before the PUT and keeps
  ambiguous admissions. It marks a ticket done when REPORT confirms its
  object, a newer object supersedes it, or its PUT fails. A background thread
  settles done tickets and a final settle runs at unmount. Per-shard session
  admission happens before ADMIT.
- **Removed per-fragment check.** Storage no longer checks the typed-writer
  lane view on every fragment. On the REPORT path that view is BUSY until
  REPORT stamps the lane. In the first cluster runs it refused 387 fragments
  and left some chunks with only two of three fragments.
- **Retired tools.** `efs-bench` remote `--store`/`--read` is refused, since
  it had no ticket. The staged unticketed live helpers
  (`client_publication_writeback.c`, `publication_replay.*`) are retired;
  `gc_reclamation.py --typed-writeback` refuses.

## Results

| Gate | Result |
| --- | --- |
| `make test` on `c860c3dbd` (intel1) | exit 0; 0 lines matching `^make(\[[0-9]+\])?: .*Error`; 412 test commands; 15 NOTRUN lines (RDMA ×10, valgrind ×2, cluster scripts, fault build, runner self-test), the same set as the baseline |
| Baseline `scripts/make_test_summary.sh` on `bf16436e8` | 1039 commands, 0 FAIL, 14 NOTRUN |
| Ticket unit tests (`tests/test_put_ticket`) | PASS: unadmitted refusal, atomic PUBLISHED, exact replay no-op, lost-CAS retry with the same ticket, body collision, ordered settle, abandon/lost-admission/GONE/retired refusal, fenced owner |
| W88 unit replay (round-6 fixture, ticketed) | PASS: no resurrection; **negative control:** the same history unticketed resurrects |
| W137 (`tests/test_live_ticket_retry.py`, now in `make test`) | PASS against replies built by the storage encoder; **negative controls fail as required:** the retired 5-byte reply shape, and a retry path that retries STALE |
| Private cluster gate `tests/live/k13_ticket_cluster.py` | **PASS twice** (gate1, gate2) on `c860c3dbd` |

Cluster gate details, identical in both runs:

- **Default PUT/flush:** seven files (1 KB, 300 KiB, 8 MiB sequential,
  64 random 4 KiB spans, rewrite, zeros) read back exactly warm with fsync.
  20 unflushed deletes and a pre-REPORT overwrite were also exercised.
- **Ticket protocol, same-group (ci 0) and cross-group (ci 1) lanes:**
  - a lost admission reply, then exact re-admission;
  - body and file collision refused, wrong bytes refused before storage;
  - lost PUT replies on every member, then the exact retry;
  - lost REPORT reply is a no-op;
  - after a crash of all daemons the admitted body and fragments survive;
  - a completed fence refuses a late PUT (STALE) and the ticket status.
- **W88 through the host:**
  - setup: eight spans, a fold, a later full image and a churn span on a
    cross-group lane;
  - the byte-identical retry of the first span is a no-op while PUBLISHED and
    STALE once retired;
  - after a crash and restart of all daemons, the same session's retry is
    STALE;
  - a foreign session's replay is refused (`reason=ticket`, the only refused
    record in the logs);
  - exact bytes read back through the mount warm and cold.
- **Leader death mid-PUT:** 64 MiB was fsynced, then SIGKILL of the node
  leading groups 0 and 2 during the next 192 MiB, and a restart after 4 s.
  - The fsync of part B returned 0 in both final runs. An earlier run of the
    same scenario got EIO here; that is expected, since W71 fences a crashed
    root's COMMIT.
  - All 256 blocks read back exact warm and after a crash of all daemons;
    no block had wrong bytes.
- **Settle:** both clean unmounts report `outstanding=0 settle_rc=0` (594–631
  batched settle RPCs per run).
- **No `pub-reject`** or invalid REPORT record appeared during the workload,
  the ticket probes, leader death or cold replay.
- **Storage refusals:** only the deliberate ones remained (the revocation
  STALE and the wrong-bytes probe).

## Commands

```bash
git archive c860c3dbd | ssh intel1 'mkdir -p ~/efs-k13/final && tar -x -C ~/efs-k13/final'
ssh intel1 'cd ~/efs-k13/final && export TMPDIR=$HOME/efs-k13/tmp && make -j8 all && make test > ~/efs-k13/maketest-final.log 2>&1; echo $?'
ssh intel1 'grep -cE "^make(\[[0-9]+\])?: .*Error" ~/efs-k13/maketest-final.log'
ssh intel1 'cd ~/efs-k13/final && EFS_PUT_TICKET_DBG=1 python3 tests/live/k13_ticket_cluster.py --source $HOME/efs-k13/final --work $HOME/efs-k13/gate1 --port 18700'
```

Each step ran in `screen` (`SCREENDIR=$HOME/efs-k13/screen`). The cluster
gate needs ports 18700–18702 free, writable `/dev/fuse` and `fusermount3`.
It takes about 3 minutes, signals only processes it started (PIDs in
`<work>/pids`) and stops them on exit. Cleanup: remove the `--work`
directory. `EFS_PUT_TICKET_DBG=1` only logs refused admissions, refused
uploads and settle batches. The baseline used
`scripts/make_test_summary.sh` in a `git archive bf16436e8` tree.

## Limitations and remaining gates

- **INCOMPLETE — D31 abandoned-upload collection.** No coordinator yet
  collects bodies after durable revocation: storage fences, deletion on
  every captured member with ACKs, and retirement. The pieces exist:
  `reclaim_begin`/`delete_ack`/`retire` and the new orphan ledger
  (`EFS_KV_KIND_PUT_ORPHAN`). Until the collector lands, abandoned and
  revoked bodies stay on disk, and they are now recorded. Live data cannot
  be collected: a PUBLISHED ticket is never abandoned.
- **Staged typed driver.** It is not activated and stays staged. The host
  refuses its unticketed requests; its client-side code and unit tests
  remain.
- **Performance not measured** (no A/B, per the host rules). Each chunk now
  costs an admission proposal, a ticket STATUS read per fragment and a
  batched settle. `efs-bench` remote store/read is gone.
- **Async-PUT contract unchanged.** A chunk acknowledged by two of three
  members is not retried to the third. A node death can make such a chunk
  unreadable until that member serves again. Unsynced data on a killed
  member can be lost and reads back EIO, never wrong bytes.
- **Simulator and metadata unit tests.** These still call the library's
  unticketed `efs_meta_apply_publish`; the host refuses it on the wire.
- **NOTRUN:** RDMA transport (no device), orcd-mini, multi-host clusters,
  hardware power loss, valgrind gates.

## Captures

Raw logs stay outside Git on intel1 in `$HOME/efs-artifacts/k13-20261023/`
(516 KiB; daemon stores were not kept):

| sha256 | file |
| --- | --- |
| `3216719fe612d303864df773bf85c31616728d9bef2113fdd1af43a3b808b842` | `maketest-c860c3dbd.log` |
| `88bf3feb4517eb5653abf0b424d944cf365e992017d175732487645758346843` | `summary-base-bf16436e8.txt` |
| `4b00ed6eb4a2c4e872d0420f1438465968b756dee6e78a0e1bc090827dc952da` | `summary-run2-worktree.txt` (pre-final worktree, 961 commands, 0 FAIL) |
| `31b892e8f1375cb986f5cff22f589eca6d887fbee546f8bb0e85789d3c1080f9` | `gate1.out` |
| `eb5b31ba4b0f73990baa9740194434dfddba18eb722ec38dae31d2ea1cdf381f` | `gate1-logs.tar.gz` (daemon, FUSE, PID logs) |
| `d7b341e8a989d200bc0d3ce1586af6f6546c3e13558ad00bc9f36d7147943ef0` | `gate2.out` |
| `5c2e152905e756832cba5c4595fb7b91b3989652116521f7cb388895bd9b61e5` | `gate2-logs.tar.gz` |

Binaries: `efsd`
`846286e067482072bf0a27fdd64952f68628fad18ae64a40b4eac7b6276a1e36`,
`efs-fuse` `6ac8c638da65d7cf36a179c0193789f7d2dd8399772370627ec5e0f3d23d8c4f`,
`efs-mgmt` `b6b1c526dca0fac239ea3ceb926c02d1611d66bc5475c0d87ae7231053844e54`,
`libefs.a` `10ca43b5134902b6cce1a36e337d0c503cb4578336b82e8d9e3f9b808671530d`.
