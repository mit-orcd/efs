# W154 rebuild on return: degraded stripes are recorded and re-protected (Oct 23, 2026)

**Purpose.** [W154](work-queue.md) / [D40](decisions.md) (alpha 1.0 scope,
Oct 23): a stripe acknowledged with two of its three fragments must be
recorded, announced, and rebuilt when the missing node returns, while
writes stay available. This closes finding F1 of the K28 five-node gate
([gates-xorinox](gates-xorinox-20261022.md#k28--w151-five-node-cluster)).
Automatic general repair (W74, K23) stays deferred: this is the one repair
in alpha, scoped to stripes acknowledged degraded.

**Verdict: PASS (scoped)** for the unit tests, `make test` on xorinox
(except the two ThreadSanitizer gates, NOTRUN there: the host lacks
libtsan) and the single-host five-daemon TCP gate, on branch `devel-w154`,
not merged. NOTRUN: multi-host, RDMA, direct I/O, orcd-mini.

## Finding (F1, restated)

A chunk PUT returns success after two fragment acknowledgements
(`put_fragments_parallel_once`, the quorum loop), and the client only
reroutes a fragment away from a node it already marks down (four failures,
then 30 s). In the window before that mark, and again each time the
cooldown runs out while the node is still away, a chunk with a fragment
placed on the absent node is acknowledged and published with two
fragments. Nothing recorded that, nothing logged it, and nothing rebuilt
it, so a later outage of a different node made the chunk unreadable.

## Change

| Piece | What it does |
| --- | --- |
| Client PUT | Per-fragment acks are unioned across the retries of one object. A two-fragment success logs one line (`put_fragments ino=… chunk=… gen=… nodes=a,b,c: degraded acknowledgement, fragment i for node n not acknowledged; published degraded for rebuild on return`; past 100/s the rest of that second is summarized), and the object goes into a bounded table until a REPORT or publication carrying it succeeds. If the table is full the write fails: a two-fragment stripe is never hidden. |
| Wire | REPORT and PUBLICATION records carry `EFS_CHUNK_REC_F_MISSING` bits (5 + i, at most one). Not part of the publication intent digest. Raft PUBLISH carries the mask in flag bits 4..6; validators refuse two missing fragments. |
| Metadata | The publication's own batch writes a degraded mark: `EFS_KV_KIND_DEGRADED` (51), the GC record key layout on the group anchor shard `(ino, object generation, lane, ci)`, value `(inode generation, missing mask, nodes, checksums)`. A full-image publication over the chunk deletes the marks of every object it supersedes (base and folded spans) in the same batch: **a rewrite retires the mark instead of racing the rebuild.** Durable and replicated like the chunk row; snapshot import carries it. |
| Finding marks | The rebuild pass walks the mark prefix of the anchor from a cursor: a durable index of marks, never a chunk-map scan. A per-anchor count (GC watermark rules: derived once, overshoots only, clamped by an empty pass) lets a rewrite skip the mark lookup when there are none. |
| Rebuild | On each group leader, inside the GC loop (about 1 s per pass): at most `EFS_REBUILD_PER_PASS` (default 256) rebuilds and 1 s per group per pass, and at most 4× that many marks examined. A mark whose missing node the server heartbeat sees down waits. Otherwise: a mark whose object the chunk row no longer references (rewrite, truncate, pack, unlink) is retired without I/O; else the two present fragments are fetched and checked against the published checksums, the third is rebuilt by XOR and checked too, PUT to the chunk map's node for that index (same object generation, so the name `{ci}.{fi}.{gen}` never collides with a newer object), and COMMITted on the incarnation that acknowledged it. Only then is `EFS_MD_CMD_DEGRADED_CLEAR` (tag 84) proposed. |
| Clear apply | Deletes the mark only when it still names the same object and fragment set (absent: idempotent no-op). For a rebuilt fragment whose object was superseded meanwhile, it queues a one-fragment GC record (or resets that fragment's ack bit in the dead object's record), so the copy is removed checksum-guarded instead of leaking. |
| Fail closed | A checksum mismatch, a missing source fragment, a failed PUT or COMMIT, a malformed mark or an unreadable row keeps the mark and logs. Peer I/O is bounded at 5 s and a failing node is skipped for 10 s. |
| Status | `efs-mgmt status` prints `Degraded chunks: N (group 0: a, group 2: b); rebuilt=… retired=… failures=…` from the group leaders (`unknown` until both leaders have reported a pass). |

Fresh format only (new KV kind, command tag and flag bits); a binary that
does not know them refuses them (W140 tag gate, flag validators).

## Tested source and build

- Code `7d189bc25`, test-harness fixes `63ed061de`, on `devel-w154` from
  devel `bf16436e8`. Tested tree: `git archive 63ed061de`.
- Build on xorinox: `make -j16 EFS_GIT_ID=63ed061deb18
  EFS_GIT_DESCRIBE=63ed061deb18 EFS_GIT_BRANCH=devel-w154 all`, gcc 14.3.1,
  x86_64, `-O3 -march=native`. Build id `63ed061deb18`.
  sha256: efsd `ba581a40e3e0f2a9a60de2b10c45af0e7ab0e3b031a81d49e1cb2a1d5b616b4a`,
  efs-fuse `592d349224fb5dac22c875e1530cea1b823321070a0ed2edfac386cab8292cc7`,
  efs-mgmt `1a82a3356c627bb29f1e2eefb121165c4b3dfc61a119300c1b9fb34e353cec8d`.
- Host: xorinox, Rocky Linux 10, 16 CPU, 60 GB RAM, stores on `/home`
  (XFS on LVM), all daemons on 127.0.0.1 with `EFS_TRANSPORT=tcp` and
  buffered fragment I/O (`--no-direct-io`), quota 4 GiB per node. The
  host's unrelated user FUSE client was not touched.

## Unit tests

- `tests/test_meta_apply.c` `test_degraded_marks`: one mark per degraded
  publication; two missing fragments refused with nothing written; replay
  adds no mark; a clean rewrite retires the base's mark; a degraded span is
  marked and the fold that supersedes it retires it and records its own;
  clear of a live object deletes the mark without GC and is idempotent; a
  clear decided for other content deletes nothing; a row disagreeing with
  its mark fails closed; a rebuild that lost the race to a rewrite queues
  its fragment again (bit reset in the live GC record, or a new
  one-fragment record after the GC retired); an unlinked object's mark is
  stale and retired; malformed rows (two-bit mask, short value, lane not
  matching the chunk) are refused. The count follows every step.
- `tests/test_erasure.c`: each of the three fragments is the XOR of the
  other two and hashes to its published checksum; argument checks.
- `tests/test_put_retry_budget.py`: a degraded acknowledgement is recorded
  and logged once; an unrecordable one fails the write.
- `tests/test_put_reply_deadline.py`, `test_publication_host.py`,
  `test_publication_rpc.py`, `test_report_frame_boundary.py`,
  `test_put_hint.py`, `test_submit_unknown_tag.py`: updated for the new
  argument, flag bits and stubs; the tag table and dispatcher agree on 50
  tags.

## `make test` on xorinox

```bash
make -i EFS_GIT_ID=63ed061deb18 EFS_GIT_DESCRIBE=63ed061deb18 EFS_GIT_BRANCH=devel-w154 test > test2.log 2>&1
grep -nE '^make(\[[0-9]+\])?: .*Error' test2.log
```

`make -i` runs every recipe line even after a failure (the `test` target is
one recipe, so a plain or `-k` run stops at the first failing line).
Build exit 0, no compiler warnings in the changed files.

Result: every test line passed except two, both
`… under ThreadSanitizer: FAIL, the TSan build did not compile` (Makefile
lines 415 and 454: `tests/test_apply_ring.py`,
`tests/test_session_latch_tsan.py`): the host has no
`/usr/lib64/libtsan.so.2.0.0` (`ld: cannot find …`). That is a host package
gap, not a source result: these two W138 gates are **NOTRUN on xorinox**,
not passed. The grep finds exactly those two
`make: [Makefile:415: test] Error 1 (ignored)` and `…454…` lines.

A first run on `7d189bc25` failed five source-extraction harnesses that
compile copies of the changed client and host functions (new argument,
missing stubs, an exact call-string assertion); `63ed061de` updates them,
and they pass in the run above.

## Live gate: five daemons, F1 scenario

`tests/live/w154_rebuild_on_return.py REL WORK [PORT]` (committed). Five
efsd on ports 19101–19105, `EFS_MD_RAFT_N=5`, nodes 2–5 join node 1,
`raft-mkfs`, one efs-fuse mount; `EFS_REBUILD_PER_PASS=4` so the rebuild
is slow enough for rewrites to race it. Files are 16 MiB (128 chunks) of
seeded random bytes, sha256 recorded, written with `close()` only (see
limitations for fsync). Stops only the PIDs it started and unmounts only
`WORK/mnt`; logs stay under `WORK/logs`.

```bash
python3 tests/live/w154_rebuild_on_return.py $HOME/efs-w154/src $HOME/efs-w154/run6 19100
```

Steps and checks: (1) 4 baseline files, 0 marks; (2) SIGTERM node 5 (it
hosts no metadata group), write 4 files at once, wait 32 s (the client's
down cooldown), write 2 more and rewrite a baseline file; all 10 read back;
the mark count equals the number of logged degraded acknowledgements and
holds for 5 s while node 5 is away; at least one degraded chunk has node 1
among its two surviving fragments (otherwise step 5 would prove nothing);
(3) restart node 5 and at once rewrite two files with degraded chunks;
(4) the count reaches 0, `rebuilt=` in status equals the `rebuilt` log
lines, fewer fragments were rebuilt than there were marks (the rewrites
retired the rest), every file reads back, and every chunk of every file
has its three fragment files `{ci}.{fi}.{gen}` on three distinct nodes'
stores (chunk map from `efs-mgmt raft-getchunks`); (5) unmount, SIGTERM
node 1 (seed, group 0 leader), remount through node 2 and read every file.

Result on `63ed061de` (run 6), **PASS**:

| Check | Result |
| --- | --- |
| Cluster, baseline | 5 up, both leaders report 0; 4 files correct; full stripes leave no mark |
| Writes with node 5 away | 10 files correct; **9 degraded acknowledgements, 9 log lines, 9 marks** (group 0: 3, group 2: 6); held at 9 while node 5 was down |
| At risk | 6 of the 9 degraded chunks had node 1 among their two surviving fragments |
| Return of node 5 | count 0; 4 fragments rebuilt (each logged, e.g. `rebuilt ino=7932 ci=5 gen=e293cdcd73021ef2 fragment=1 on node 5 (from nodes 1,4)`), 5 marks retired in the racing rewrites' own batches; status `rebuilt=4 retired=0 failures=2` |
| Placement | 1280 chunks of 10 files: three fragment files on three distinct nodes |
| F1 | node 1 stopped, cold remount through node 2: all 10 files correct |

The 2 failures are rebuild attempts before the server heartbeat had marked
node 5 down (`PUT failed (rc=-6); mark kept`); the marks waited and were
rebuilt after the return. Development runs 1–5 on uncommitted trees found
the issues recorded under limitations (hung node, crash restart, async
publication timing in the harness); run 5 on the same code as `7d189bc25`
also passed (12 marks, 8 rebuilt, 4 retired by rewrites, 9 at risk).

## Limitations

- One host on loopback TCP: five daemons are not five machines.
- **fsync with a node down returns EIO by existing design**: DATA_COMMIT
  goes to every storage node (`efs_client_data_commit`). The gate therefore
  publishes with `close()`. Write availability under D40 holds for
  publication; an fsync-available degraded mode is a separate question
  (follow-up, not W154).
- **A node restarted after a crash (SIGKILL) fences COMMIT** ("unclean
  previous stop", existing behaviour until verified recovery or a fresh
  format). A development run that killed node 5 showed the rebuild failing
  closed there: every rebuilt fragment's COMMIT was refused, the marks were
  kept and each attempt logged `PUT failed … mark kept`. Rebuild onto such
  a node waits for that recovery. The gate uses a clean stop.
- A hung (SIGSTOPped) node was tried in development: the client's REPORT
  timed out after 30 s and `open()` returned EIO. That gray-failure path
  is outside W154 and was not investigated; the rebuild's own peer I/O is
  now bounded at 5 s.
- The degraded acknowledgement window is the client's down detection; the
  gate counted 6–12 degraded chunks per run, not hundreds.
- Packed segments are written only with three acknowledgements (the packer
  cancels otherwise) and are not marked. The staged ticketed PUT path
  (`EFS_MSG_PUT_TICKETED`) is not covered by the degraded table.
- A mark whose missing node never returns stays (fixed membership, D39)
  and stays visible in the status count; there is no re-placement onto
  another node (W74).
- No A/B run against a build without the rebuild: the gate shows the
  at-risk chunks (degraded with node 1 among the survivors) exist and read
  correctly after node 1 stops.

## NOTRUN

Multi-host, RDMA, direct I/O, orcd-mini, scale (many degraded chunks, a
three-node cluster with one node down for a long write), crash of a
leader during a rebuild.

## Raw outputs

Kept on xorinox under `$HOME/efs-artifacts/w154-20261023/` (the work
directory `~/efs-w154` was deleted):

| File | sha256 |
| --- | --- |
| `build.log` | `8a9e131cbf82d05fae0d431d8ac2cdd18dae1c54c29a537447f701859b9c24b8` |
| `make-test-i.log` | `eeea747c334203907b9b43a3b82ccfee47ca42ab80a97fc0a3ff1b44b8da75bb` |
| `live-gate-run6.out` | `e5e1ead3a591312a6e8768d840e86f6429aa99608c5b8ae866312514f2120627` |
| `live-gate-run6-degraded-acks.txt` (9 client lines) | `fbd7effb7cd1ec45d75eb238072a111ffebe20a75bc0c9cd73c09c19e9e2e2fe` |
| `live-gate-run6-rebuild-lines.txt` (6 server lines) | `df20d512ead127b5194ed91eae7b0698f331e4353e0e0c8ba7a6a44c36fd4e73` |
