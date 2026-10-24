# K37 · rebuild on return for packed segments (Oct 24, 2026)

**Purpose.** Checklist item K37 in
[alpha-1.0-2plus1-checklist.md](alpha-1.0-2plus1-checklist.md) (section K).
Alpha 1.0 requires every acknowledged write to survive any single-member
loss once the cluster has restored protection. W154 re-protects an ordinary
chunk stripe acknowledged on two fragments; K36 found that a packed segment
(D38: one 2+1 stripe holding up to 512 small files) with a missing fragment
was only counted, never rebuilt, so it stayed one loss away from losing
every packed file in it. K37 gives segments the same rebuild on return.

**Verdict: PASS (scoped, loopback regression evidence).** intel2 `make
test-all` at `b205cb9db`: 459 commands, 1 FAIL that also fails on the base
(`tests/test_get_integrity.py`), 14 NOTRUN host limits. Branch
`claude/devel-pack-rebuild-fa5139` on `merge-wave5` (`376369aa8`: devel plus
READ_VERIFY, K34, the A7 removals and K35), not merged, not pushed. Code
commits `b48ec444f` (the change), `5ef2b8d6d` (the audit pages its index
walk) and `b205cb9db` (`tests/test_k33_payload.py` waits for segment
protection); the gate ran on `b205cb9db`. Live multi-host, RDMA and orcd-mini verification:
**NOTRUN** (parent session). Single-host loopback daemons are regression
evidence, not qualification.

## How a segment loses a fragment, and what now records it

| Path | Before | Now |
| --- | --- | --- |
| Written while a member is down (the window before the heartbeat marks it) or refusing (quota, fence) | The packer cancelled any copy without three acknowledgements; nothing was written degraded. | Two fragments durable (each fsynced before its acknowledgement) and the third not: the packer commits a degraded mark (`R_WRITE`) and only then switches files in. One durable fragment, or a mark that does not commit: cancelled as before. |
| A member returns without its fragment (deleted, store replaced) or with a torn or corrupt copy | Not detected. Reads used the other two fragments silently. | Audit on return: the group leader sweeps every member that comes back with an incarnation no leader of the group has swept; the member reads and verifies every fragment placed on it (`MISSING` / `BAD` are marked, `R_MISSING` / `R_BAD`). |
| K36 verified recovery drops an unprovable segment fragment | Counted only. | The same audit: a member answers only once its stores are settled (no root recovery-uncertain), so a fragment that recovery dropped is missing when the sweep runs. K36 can also mark directly with reason `R_RECOVERY` (defined, unused until K36 merges). |
| Compaction with a member down | Same as packing (cancelled without three). | Compaction writes through the same copy path: marked, rebuilt on return. The old segments' records are released by the switch and their marks retire with them. |
| A second loss before the rebuild | — | A rebuild source that its holder proves `MISSING` or `BAD` adds its bit (`R_SOURCE`): two bits is **unrecoverable**, reported and never cleared by a rebuild; the files read `EIO` until rewritten or removed, when the segment's retirement removes the mark. |

## Change

| Piece | What it does |
| --- | --- |
| Authority | One row per segment: KV kind **249** `EFS_KV_KIND_PACK_DEGRADED` on the group's anchor shard, keyed like the segment object `(anchor, id, generation)`, value `[mask][reasons][0:2]`. The segment object stays the authority for placement and checksums; the mark is the only authority for "fragments missing". Written only through Raft tag **247** `EFS_MD_CMD_PACK_DEGRADED` `[group][op][id:8][gen:8][mask][reason]` (pack family: byte 1 is the group, applied only by that group): MARK adds bits on an ACTIVE segment (STALE otherwise); CLEAR deletes a one-bit mark that still names exactly that fragment; RETIRE deletes a mark whose segment is gone or holds no live or pending record; `PACK_RETIRE` deletes the mark before the object in the same apply. A malformed mark row is reported and never overwritten. |
| Packer | `pk_segment` (packing and compaction): `nok == 2` and `server_segment_mark` OK (the proposal committed and the leader's replica holds the bit) → switch; else cancel. Logs `packer: segment g… id=… gen=… written degraded: fragment f for node n not acknowledged (rc=…); marked for rebuild on return`. |
| Audit | Wire **241/242** `EFS_MSG_SEGMENT_AUDIT`. Each group leader probes each available member at most every 2 s (incarnation, audited-groups bit, settled). An unswept, settled member gets a sweep of the group's published segments (ACTIVE, no PENDING record) from a cursor, at most 256 fragments per member per pass. The member's verdict per fragment: file length = payload + 4 KiB tail, I25 record names exactly that fragment, recorded digest and payload hash equal the segment checksum (`OK`), no file (`MISSING`), anything else wrong (`BAD`), I/O error (`ERROR`, proves nothing). A sweep with any `ERROR` or unrecorded mark never completes and starts over after 30 s; a complete sweep is recorded by the member for its incarnation, so a new leader does not repeat it. |
| Rebuild | `src/server/segment_rebuild.c`, from the GC loop after W154's pass: at most 64 rebuilds and 1 s per group per pass, a node whose I/O failed skipped for 10 s. Retire a mark whose segment is no longer live; report a two-bit mark (`UNRECOVERABLE`, at most once a minute); wait until all three placement nodes are available; read the two present fragments (local or `GET_SEGMENT`) and check them against the segment checksums; XOR the third and check it; `PUT_SEGMENT` with header byte 2 `EFS_SEGMENT_PUT_F_REBUILD`; the storage node accepts it only while `PACK_OBJECT_READ` returns the object ACTIVE with live records and the mark mask exactly `1 << fi` beside it (the reply's former reserved word), replaces a torn or foreign copy, fsyncs file and directory before replying; audit that one fragment on its node (read back and verified there); only then CLEAR. |
| Status | `efs-mgmt status`: `Degraded segments: N (group 0: a, group 2: b); unrecoverable=U rebuilt=… retired=… failures=… audit_checked=… audit_marked=…` (`unknown` until both leaders reported a pass); `Cluster state: DEGRADED` while N > 0 with every member up; `Protection: DEGRADED: … chunks and … packed segments …` or `Protection: LOST: U packed segments lost two or more fragments …`. |
| Client | A packed read that found fewer than two fragments returned the storage node's `NOT_FOUND`, so `read()` on an open file failed `ENOENT`; it is `EIO` now. |
| Fault hooks (`EFS_FAULTS` only, A6 rows added) | `EFS_FAULT_PACK_PLACE_DOWN=<id>` places segments on that node even while it is marked down; `EFS_FAULT_SEGMENT_PUT_REFUSE=<dir>` makes node N refuse packer PUTs while `<dir>/refuse-N` exists; `EFS_FAULT_SEGMENT_REBUILD_HOLD=<file>` keeps marks waiting while the file exists. |

New numbers (high end of the free ranges; recorded in the shared notes so
parallel sessions avoid them): KV kind 249, Raft tag 247 (248 left for K36's
`RECOVERY_MARK`), wire 241/242 (K36 has 243/244). `tests/test_kv_kind_unique.py`
PASS. Fresh format only; a build that does not know tag 247 refuses it
(W140 tag gate).

## Packing degraded, and the window it opens

`tests/test_k33_payload.py` showed how often the degraded copy happens:
in 3 of its 5 single-member outages the packer copied a segment in the
seconds before the heartbeat marked the stopping member down, so the copy
was written on two fragments, marked, and rebuilt when the member returned
(both runs below: 3 `written degraded` lines and 3 rebuilds each). While a
member is down, every stripe with a fragment on it is already at two
available fragments, so a segment written degraded during the outage is no
worse during it; the extra exposure is the time between the member's
return and the rebuild (seconds), the same window W154 accepts for chunks
acknowledged degraded. A test (or an operator) that takes the next member
down must wait for protection first: `test_k33_payload.py` waited only for
`Degraded chunks: 0` and failed once in the first gate attempt (EIO on a
packed file during the next outage, `tmp/fail-412.log` kept in the raw
outputs); it now also waits for `Degraded segments: 0` and passed 2 of 2
dedicated runs (283 s, 133 s) and the gate. The stricter alternative,
cancelling any copy without three acknowledgements (the behaviour before
K37), never lowers protection at the cost of packing a few seconds later;
it is a one-line policy change in `pk_segment` if the user prefers it.

## Tests

Unit (in `make test` / `make test-long`):

- `tests/test_pack_switch.c`: malformed commands write nothing; an unknown
  or reclaiming segment takes no mark; MARK, replay, CLEAR of another
  fragment refused, RETIRE of a live segment refused, proven CLEAR and its
  replay; a second loss gives two bits that no CLEAR or RETIRE removes while
  the segment is live; `PACK_RETIRE` deletes the retired segment's mark and
  keeps an ACTIVE one's; a malformed mark row is `PROTO` and never
  overwritten.
- `tests/test_reclaim.c` (`K37 segment audit and rebuild PUT`): fragment
  verdicts OK / MISSING / BAD (truncated tail, corrupted payload byte); the
  audit exchange (incarnation check, settled, COMPLETE only empty and only
  for this incarnation, reserved fields refused); the rebuild PUT accepted
  only with the exact one-bit mark of a published segment, refused unmarked,
  for another fragment, with two bits or an unreadable mark; it replaces a
  torn or corrupt copy and a retry is idempotent; the packer path still
  needs pending records.
- `tests/test_pack_object_remote.py`: the mark mask rides the remote and
  local object read; unreadable mark → `EFS_PACK_READ_DEGRADED_BAD`;
  refusals leave the mask BAD.

Daemon test `tests/test_k37_segment_rebuild.py` (`make test-long`): three
efsd (`tests/faults/efsd`) and efs-fuse on loopback, packer minimum age
500 ms, scheduled compaction moved out of the run.

| Step | Checks |
| --- | --- |
| 1 baseline | two batches of 8 small files (1 B–16 KiB) packed with all up; no mark; `Protection: full` |
| 2 deleted and torn while down | node 2 stopped; one of its segment fragments deleted, another truncated to 4 KiB; on return the leader's audit marks exactly 2 (`missing`, `torn or bad`), both rebuilt (files back, full length); every file reads back through a fresh mount on node 3 |
| 3 written with a member down | node 3 stopped (`EFS_FAULT_PACK_PLACE_DOWN=3`); new files packed into segments written degraded, each with node 3 named; `Degraded segments` ≥ that count, `Cluster state: DEGRADED`, `Protection: DEGRADED`; files read with node 3 down; node 3 returns with the rebuild held: `Cluster state: DEGRADED (3 up, 0 down)`; released: count 0, every rebuilt fragment file present on node 3, `Protection: full`, `Cluster state: OK`; fresh mount on node 2 reads every file |
| 4 compaction with a member down | half the packed files removed (holes); node 3 stopped; `efs-mgmt pack-compact` on both leaders; the output segment is written degraded and marked; files read with node 3 down; on return rebuilt (file present on node 3), every file exact |
| 5 never on fewer than two | node 3 down and node 2 refusing segment PUTs: the copy fails with one durable fragment (`put_errors` +1), `files_packed` unchanged, no degraded write, no mark; files read back; packed once node 3 returns |
| 6 single-member outages afterwards | each of nodes 1, 2, 3 stopped in turn; a fresh mount through another member reads every file exactly |
| 7 second loss | node 3 down, a new batch packed degraded, then each such segment's node 2 fragment deleted; node 3 returns: each reported `UNRECOVERABLE`, `Degraded segments: n … unrecoverable=n`, `Protection: LOST`, `Cluster state: DEGRADED (3 up, 0 down)`, no rebuild logged for it after 5 s; its files read `EIO` (fresh mount), all others exact; removing its files retires the marks (`Protection: full`) |

Development run 4 on the code of `b48ec444f` (intel2): **all 7 steps PASS
in 164.2 s**; the gate run on `b205cb9db` is in the gate section. The K37 log lines of that run (51 lines from the three
daemons) show the audit marking the deleted fragment (`fragment 1
missing`) and the truncated one (`fragment 0 torn or bad`), the storage
node replacing the bad copy before the rebuilt write, and every rebuild with
its source nodes, e.g. `rebuilt g0 id=… gen=1 fragment 1 on node 2 (from
nodes 3,1)`. Earlier development runs failed only on test-harness issues,
fixed before `b48ec444f`: the K35 helper `packed_total` sums the
process-wide counter once per leader term (double counting after a leader
change; the test now reads node-wide counters from `efs-mgmt pack-compact
<ep> 0`), `files_cancelled` is not counted on the PUT-failure path (the
test waits for `put_errors`), and the `ENOENT` read above (fixed in the
client).

## Gate: `make test-all` on intel2

- **Tested source:** `b205cb9db` (all code and tests of this change; the
  commit after it touches only documentation), copied with `git archive`
  into a private tree on intel2. Build `EFS_GIT_ID=b205cb9db`, gcc 14.3.1,
  x86_64, `-O3 -march=native` (Makefile defaults), `EFS_BUILD_JOBS=4`.
  sha256: efsd `1aba3f8204e71a473f4119e255d22b929a8c45190bd0060a4e1e9d27758a9a09`,
  efs-fuse `13d1a2fdfe08128a53a27c3e39de45e6e90af267bd6fcca27b19c58e66621a68`,
  efs-mgmt `2deef2129bc1218a12b53f4516940e2a94b2dec0b4aec2293995c60adedb5e98`,
  `tests/faults/efsd` `a01da59fba4ffd520dd388ba1afbc763acc14b26f9823acfafdac5aea540d4c8`.
- **Host:** intel2 (8 CPUs, 15 GB, Rocky Linux 10.2, kernel
  6.12.0-211.67.1.el10_2), loopback TCP only (no RDMA device), daemons on
  test-chosen loopback ports, stores and `TMPDIR` under `$HOME`, direct
  fragment I/O. The user's cluster members on intel2 (efsd on 17432/17433,
  the `/data1/efs` mount) were not touched.
- **Command:**

  ```bash
  EFS_GIT_ID=b205cb9db EFS_GIT_DESCRIBE=b205cb9db \
  EFS_GIT_BRANCH=claude/devel-pack-rebuild-fa5139 EFS_BUILD_JOBS=4 \
  scripts/make_test_summary.sh      # make test-all, every command, one at a time
  ```

- **Result:** `TOTAL 459 FAIL 1 NOTRUN 14 STATUS FAIL (1593s) BUILD 29s`.
  - The one FAIL, `tests/test_get_integrity.py`, also fails on the base
    `376369aa8` the same way (run there on the same host: the test's
    extracted copy of `efs_client_decode_placed_chunk_attempts` calls K34's
    `read_reprobe_down`, which the test does not supply: `implicit
    declaration of function 'read_reprobe_down'`). It is a merge
    interaction of READ_VERIFY's test and K34's `read.c`, not K37 (K37's
    one-line change to that function comes after the call).
  - NOTRUN, all host limits: no `~/git/cluster` (`test_cluster_stop.py`),
    no valgrind (`test_rdma_memcheck.py`, `test_leak_gate.py`), no RDMA
    device (`test_declaration_gate` RDMA path, `test_rdma_xprt`, seven
    `test_rdma_segmented` modes), `test_follower_bound`'s compactor case
    (needs a whole `EFS_FAULTS=1` build), and the runner's own self-test
    line that prints NOTRUN by design.
  - K37 tests in that run: `tests/test_k37_segment_rebuild.py` PASS
    (164.2 s), `tests/test_k33_payload.py` PASS (207.6 s),
    `tests/test_k35_fsync_degraded.py` PASS, `./tests/test_reclaim` PASS,
    `./tests/test_pack_switch` PASS, `tests/test_pack_object_remote.py`
    PASS, `tests/test_kv_kind_unique.py` and `tests/test_env_inventory.py`
    PASS.
- **Earlier attempts (superseded):** a gate on `b48ec444f` was stopped
  after 412 of 459 commands (1 FAIL, `test_get_integrity.py`) to add the
  index paging; one on `5ef2b8d6d` was stopped after 414 commands when
  `test_k33_payload.py` failed (section "Packing degraded"). Their partial
  progress and that failure log are in the raw outputs.
- Docs gate: `cd docs && python3 gen-architecture-full.py` and
  `python3 docs/check-architecture.py`: all 5 checks passed.

## Limitations

- One host on loopback TCP: three daemons are not three machines.
- The audit runs when a member returns with a new incarnation (and once per
  member after a cluster-wide leader change only if its sweep did not
  complete). A fragment lost on a member that never restarts is found only
  when a rebuild needs it as a source, or by a read (which still succeeds
  from the other two). Continuous scrubbing is general repair (W74, K23:
  deferred past alpha).
- A store whose recovery stays uncertain (an unclean stop without K36) is
  never audited; its fragments are trusted as before until K36 settles it.
- A mark whose placement node never returns stays visible; there is no
  re-placement onto another node (W74).
- No crash of the leader in the middle of a rebuild was injected; the
  steps are idempotent by construction (a good copy already present is
  success, a CLEAR applies only to the same mask) but not tested as such.
- `packed_total` in `tests/test_k35_fsync_degraded.py` has the per-term
  double counting described above; that test only needs a lower bound, so
  it is left alone.

## NOTRUN

Multi-host, RDMA, orcd-mini, five or more members (segments on
storage-only members), scale (many degraded segments, long outages), leader
crash during a rebuild, K36 integration (merged later).

## Reproduce

```bash
make -j8 all tests/faults/efsd tests/test_pack_switch tests/test_reclaim
./tests/test_pack_switch
./tests/test_reclaim
python3 tests/test_pack_object_remote.py
python3 tests/test_k37_segment_rebuild.py   # needs /dev/fuse and fusermount3; ~3 min
```

`EFS_K37_KEEP=1` keeps the daemons' logs and stores under `TMPDIR`; delete
the kept directory afterwards.

## Raw outputs

Kept on intel2 under `~/efs-artifacts/k37-20261024/` with `SHA256SUMS`
(not in Git): `SHA256SUMS` sha256
`61fbd81deb4266418e9a43ab46bbcd0eab24df2dface75621b765a53ec98d9f0`, 17
files, 212 KB: the gate's `summary.txt`, `progress.txt`, `timings.tsv`,
`build.log`, `cmds.txt`, runner exit and the `test_get_integrity.py`
failure log; the base run of that test; the stopped `5ef2b8d6d` attempt's
progress and `test_k33_payload.py` failure; the two dedicated
`test_k33_payload.py` runs (output and every K37 daemon log line); the
development run 4 of `tests/test_k37_segment_rebuild.py` (output and its 51
K37 daemon log lines). The private trees (`~/efs-k37`) were deleted.
