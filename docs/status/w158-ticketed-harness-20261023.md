# W158: live harnesses send ticketed PUTs (K13 drift)

**Verdict: PASS (scoped) on branch `devel-w158` (`51e884644`), not merged.**
Every hand-built PUT in `tests/live/` now carries the K13 ticket header,
and each harness checks that an unticketed PUT is refused. `make test-all`
passes on nuc (exit 0, no `make … Error` lines). The three converted wire harnesses and a private
four-member check of the `gc_reclamation.py` late-PUT steps pass on nuc.
**NOTRUN:** the full expiry fixture (`tests/posix/posix_expiry.py` →
`gc_reclamation.py --expiry --posix`) on the intel cluster, which is the
parent session's rerun; RDMA; multi-host.

- **Purpose:** [W158](work-queue.md), found in the
  [intel run](intel-devel-a23d45540-20261023.md) (defect 3). After
  [K13](k13-w86-ticket-default-20261023.md) storage stores a fragment only
  for an ADMITTED ticket (fresh ReadIndex STATUS) and refuses an unticketed
  PUT with reply status 1. The expiry fixture's "valid late-PUT control"
  still sent the pre-K13 native header, so it stopped after 91 passing
  checks, before its expiry checks and its own POSIX run. The server's
  refusal is correct and is not changed.
- **Tested source:** `51e884644` on devel `fc4561edf` (one commit: scripts
  and tests; this page is the docs commit). Build tree `git archive
  51e884644`, so the build ID reads `unknown`. sha256: `efsd` `d5edf967…c27546a`, `efs-fuse` `fe08c749…cb699d16`,
  `efs-mgmt` `911130a0…7a23a69d1`, `libefs.a` `f792096b…2f4b8f`.
- **Environment:** nuc (user maik), Rocky Linux 10.2, kernel
  6.12.0-211.67.1, 8 CPU, gcc 14.3.1, Python 3.12.15. Work tree, stores and
  `TMPDIR` under `$HOME/efs-w158`. Private daemons on 127.0.0.1:18900–18913
  only, `EFS_TRANSPORT=tcp`. The user's live cluster on 17432–17435 and its
  mounts were not touched.

## Sites found and what changed

Searched `tests/live/`, `tests/measure/`, `tests/faults/` and `scripts/` for
raw `EFS_MSG_PUT_CHUNK` (6) frames, the pre-K13 `Put` header
(`eid, ino, ci, fi, sum, length, gen, hint`), the retired `PUT_TICKETED`
opcode (129/130) and the old reply. C helpers in `tests/live/` already use
`efs_put_upload_header` (`client_put_ticket.c`); nothing in `scripts/`,
`tests/measure/` or `tests/faults/` builds a PUT frame; no sender of 129/130
is left.

| Site | Before | Now |
| --- | --- | --- |
| `tests/live/gc_reclamation.py` `valid_put` — late-PUT control (fault step) | unticketed PUT to all four members, expected OK (refused since K13) | ticket admitted through the production ledger, fragment fi uploaded to its placed member, OK on all three; **new check:** every member refuses an unticketed PUT (status 1) and stores nothing |
| `tests/live/gc_reclamation.py` — tail "late PUT fenced" after restart | unticketed PUT expected status 1: passed trivially since K13, proved nothing about the retired inode | a ticket admitted while `offline-member` lived is replayed after its retirement and restart: every placed member refuses it; a fresh admission is refused; unticketed still refused; no fragment |
| `tests/live/fragment_integrity.py` (in `gate_rounds.py`, `nuc_closure.py`) | one unformed daemon, unticketed PUT/retries/wrong digest | formed three-member cluster per configuration; ticketed fragment 0; refusals: unticketed, wrong length, truncated, other bytes, tampered ticket; ambiguous PUT + 20 hintless retries keep one object and charge; restart of the member |
| `tests/live/fragment_tail_smoke.py` | one unformed daemon, unticketed PUT | formed cluster, ticketed fragment 0, direct I/O tails |
| `tests/live/io_stats_smoke.py` | one unformed daemon, unticketed PUT, retired GC_FRAGMENT (99) delete | formed cluster, ticketed PUT/GET, missing read after unlink, one reclaim removal |

The last three were also stale beyond K13 and are updated, not just made to
pass: since K16/I25 (Oct 22) a fragment ends in a 128-byte record (buffered)
or a 4 KiB page holding it (direct), and a refused record is reported as
`INTEGRITY` (3), not `NOT_FOUND` (1); the single-fragment `GC_FRAGMENT`
request (99) was retired on Oct 21, so `io_stats_smoke.py` now removes the
fragment by file unlink and node reclaim (W124). The duplicate-GC-request
check it had is gone with that RPC.

New shared pieces:

- `tests/live/ticket_put.py`: builds the K13 frame (be32 export, be32
  fragment, 216-byte ticket wire, `fragment_len` bytes), the unticketed
  refusal-control body, and `Cluster` (a disposable formed membership).
- `tests/live/client_put_ticket.c`: modes `create`, `unlink` (no mount
  needed) and `admit` (production `efs_client_put_admit`, saves the ticket
  wire and the three fragments, prints the verdict either way).
- `tests/live/ticket_put_cluster.py`: private four-member check of the two
  changed `gc_reclamation.py` steps, with the same calls and
  `EFS_PROBE_NODES=4`.
- `tests/test_live_ticketed_put.py` (fast `make test` group): fails if a
  live harness or script defines the pre-K13 PUT header, sends a type-6
  frame whose body is not a ticket body, uses the retired PUT_TICKETED
  opcode, or builds an unticketed body without asserting refusal (status 1);
  C helpers must send PUT_CHUNK as header + data parts with
  `efs_put_upload_header`.

## Tests

| Check | Command (nuc, `~/efs-w158/src`) | Result |
| --- | --- | --- |
| Static test, negative control | `python3 tests/test_live_ticketed_put.py <tree of fc4561edf>` | **FAIL as intended:** pre-K13 header and unticketed frames in all four harnesses; with only the old `gc_reclamation.py` it reports `pre-K13 PUT header` and line 541 |
| Static test | `python3 tests/test_live_ticketed_put.py` | PASS, 14 sites |
| Full code gate | `TMPDIR=$HOME/efs-w158/tmp make test-all` | **PASS:** build exit 0, `make test-all` exit 0 (about 4.5 minutes), 0 lines matching `^make(\[[0-9]+\])?: .*Error`; the log's `FAIL` lines are the expected negative-control output of passing tests; `test_live_ticketed_put.py` reports 14 sites |
| Wire harness (rerun on the `git archive 51e884644` tree, as all rows below) | `python3 tests/live/fragment_integrity.py` | PASS: buffered/direct × one/two roots, every check above |
| Wire harness | `python3 tests/live/fragment_tail_smoke.py` | PASS: padded tail, 128 and 4096 tails, 127/0 refused, payload/record corruption refused |
| Wire harness | `python3 tests/live/io_stats_smoke.py` and `--mode direct` | PASS: `disk_read ops=2 bytes=65536 errors=1`, `put_chunk ops=2 errors=1`, `removed_fragments=1 reclaimed_payload_bytes=65536`, `gc_io=unlink ops=1` |
| Late-PUT steps, four members | `python3 tests/live/ticket_put_cluster.py --work ~/efs-w158/tmp/w158-late-put --port 18910` | PASS in 9 s: OK on placed members [1, 2, 3]; unticketed refused on all four, nothing stored; unlink reclaims the uploaded, never-reported fragments; after restart of all four the held ticket is refused `STALE` (4) on every placed member, fresh admission `NOT_FOUND` (-3), unticketed refused |
| Expiry fixture | `posix_suite.py … --suite expiry` (intel) | **NOTRUN** (parent's rerun) |

Before the four-member check, a three-member probe (not committed) showed
the refusal statuses the harnesses now assert: unticketed, truncated, other
bytes and tampered ticket all reply status 1; a GET of a missing generation
replies `NOT_FOUND`.

## Limitations and remaining gates

- The `gc_reclamation.py` changes are checked statically, by the private
  four-member check of the same calls and by `py_compile`; the fixture
  itself was not run here. The intel expiry rerun decides W158.
- The fault step's ticket stays ADMITTED and unreported (like the old
  extra generation): the four-member check shows unlink reclaims its
  fragments, but in the fixture it also meets the crash-with-pending-record
  path, seen only in the rerun.
- Ticket helpers connect over TCP; RDMA was not exercised.
- Cleanup: `~/efs-w158` on nuc removed after the run; no process left.

## Raw evidence (outside Git)

None retained: the `make test-all` log and the harness outputs were read on
nuc and summarized here; `~/efs-w158` was deleted afterwards.
