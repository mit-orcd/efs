# Review of `claude/pack-validate-once` — 2026-10-22

**Verdict: merged into devel in `842a014b3` by user decision (October 22),
with the modify-under-load read mismatch still OPEN (below).** The merged
devel passes `make test` 819/819 on node9901 (clean `git archive` of
`842a014b3`, `scripts/make_test_summary.sh`, `ASAN_OPTIONS=detect_leaks=0`).
The cluster gates and the mismatch investigation listed below remain NOTRUN
on devel. The branch is deleted; its commits are in devel's history. Subject: commits `207dde6b4`, `660c6621c`, `745af27f5`, `e223b88d5`,
`7cef019b4` and `1df84466d` on `claude/pack-validate-once`, based on devel
`dd43b4015`. They fix findings 1 and 2 of
[the ORCD tiny-file profile](orcd-tiny-profile-20261022.md). The original
review below predates that merge.

## Code review

- **Validate once (`efs_pack_view`).**
  - `efs_pack_view_open` validates the image once and seals the image
    pointer, geometry and header.
  - `efs_pack_view_read` checks segment identity, then binary-searches the
    record at the reference's offset. It compares every reference field,
    including the digest, and hashes that record's payload again.
  - Offsets strictly increase in a validated image (contiguous, non-empty
    records), so finding the record by offset accepts exactly the references
    the old search by inode accepted, with the same error codes.
- **Duplicate-inode checks.**
  - Ledger init drops its pairwise inode check. This is safe: references must
    have strictly increasing offsets, each must match a record of a
    validated, inode-unique table, and their number must equal the record
    count.
  - Validation now checks inode uniqueness by sorting (a 4 KiB stack array
    for 512 records) instead of comparing every pair.
- **Public read unchanged.** `efs_pack_read`, used once per read on the client
  path, still validates on every call, as before.
- **Packed bases (`pk_read_bases`).**
  - Reads are grouped by segment and fetched by worker threads, and each file
    gets its own result.
  - If the grouping allocation fails, packed-base files fail rather than
    being read the old way: those files skip packing for that pass.
- **Abandon scan (`efs_pack_abandon_scan`).**
  - The pre-decode filter reads the encoded offsets for count, phase, owner,
    epoch, live and dead. These match `efs_pack_ledger_encode`, and "pending"
    equals count minus live minus dead, as in `efs_pack_ledger_usage`. The
    unit test builds objects with the real encoder, so the offsets are
    exercised.
  - Resuming after a work-bound stop restarts strictly after the last
    consumed key, which matches the `efs_kv_lsm_scan_work` contract.
  - A term change restarts the sweep from the beginning.
  - Minor: no test drives the work-bound (tombstone) stop. The tests cover
    only callback stops.

## Reviewer's own run

The branch was built from a clean `git archive` of `1df84466d` on node9901
(gcc, RHEL), with `ASAN_OPTIONS=detect_leaks=0`.
- **PASS:** `test_pack_validate_once`, `test_packed_segment`,
  `test_pack_ledger`, `test_pack_ledger_recovery`, `test_pack_switch`,
  `test_packed_reader`, `test_kv_lsm` and `test_pack_base_reads.py`.
- **Builds:** `efsd`, `efs-fuse` and `efs-mgmt`.
- **Not run:** the cluster gates. The C tests do not build on macOS
  (`log_ts.c` uses `fopencookie`).

## Open item blocking merge

The branch's modify-under-load run 1 returned wrong bytes three times
(correct size) on `w2/d049/f004799` v6, a 3.6 MB file. The later final
verify was clean. Two other branch runs and both base runs were clean.
- **"No changed code path" does not hold.** The branch doc says no changed
  code path can reach a 3.6 MB file. But `w125_concurrent.py` modifiers
  append and rewrite, so the file may have been packed while small and then
  grown. A client then reads chunk 0's packed base through `efs_pack_read`
  (`src/client/read.c` → `packed_reader.c`), which this branch changed, and
  overlays the spans. The packer's repack of modified packed files
  (`pk_read_bases`) changed too. Whether the file's earlier versions were
  packed is unknown.
- **The evidence is gone.** That run's storage and raw output were deleted,
  so the event cannot be traced, and an unresolved wrong-bytes finding must
  stay traceable (AGENTS.md).
- **The branch is still a plausible cause.** Analysis suggests the new read
  returns OK only for a record whose payload hash matches the reference
  digest, which would make it unlikely to produce wrong bytes. That is not
  evidence, and 1 failing run out of 3 against 0 out of 2 cannot clear the
  branch.

**Required before merge:**
1. Repeat `w125_modify_repro.sh` at least 5 times on each build, alternating
   builds. Keep the storage and `fuse.log` of any failing run outside Git,
   and record their location and hashes.
2. For any mismatch, run `w125_modify_inspect.py` on the kept storage.
   Record whether the file's chunk 0 has a packed base and which record and
   generation the read used.
3. If it also reproduces on the base build, file it as a devel client-read
   bug, separate from this branch.

## Evidence-summary gaps

- **Unclear tested build.** The private-cluster gates ran on a release tree
  patched with a diff, under a build label of an earlier hash. Rerun the
  merge-candidate gates from a clean archive of the final commit, with
  sha256 values recorded.
- **"macOS" claim.** "Unit evidence (macOS and node9900)" cannot include the
  C tests, which need Linux. Say which tests ran on macOS.
- **No raw output.** "Raw output was not kept" is fine for passing runs but
  not for the failing modify run; see above.

## Independent assessment of `15a2c957d` — October 22

**Agree: keep the merge blocked.** The performance diagnosis remains supported,
but the unresolved wrong-byte event prevents correctness acceptance. This
assessment inspected the review and the relevant source/script at `1df84466d`;
it did not rerun the Linux tests or reproduce the mismatch. Their PASS results
remain attributed to the reviewer above.

### Corrections to the conclusions

- Replace the blanket interpretation of “code correct” with “no defect identified
  in the reviewed mechanisms; focused tests PASS; concurrent correctness remains
  unresolved.” Inspection and unit tests do not prove that the integrated read
  path is correct under modification.
- “Public read unchanged” means its validation contract is intended to remain
  unchanged, not its implementation. `efs_pack_read` now calls
  `efs_pack_view_open` and `efs_pack_view_read`, so it is part of the changed
  production path. The file-size argument does not exclude it.
- A matching record digest proves integrity relative to the supplied reference,
  not that the reference is the right version for this read. Stale mappings,
  base/span version mixing, invalidation races or incorrect workload expectations
  can return internally valid bytes inconsistent with the expected file version.
  Diagnose the reference selection and assembled read, not just segment hashes.
- The view seal binds pointer/geometry/header; it does not pin memory or enforce
  immutability. Review ownership and lifetime through grouped worker reads and
  returned payload pointers. This is a required invariant, not a demonstrated
  defect in the branch.

### Make the reproduction preserve failures first

At `1df84466d`, `scripts/w125_modify_repro.sh` stops and removes
`/tmp/w125-modify-PORT` at the start. Reusing the same port therefore destroys the
previous storage. Give every attempt a unique retained run directory or preserve
and inventory the failed stopped store before any subsequent run. Keep logs,
expected/actual payloads and hashes, workload seed/options, exact build hashes,
operation history and the mapping/base/span identities associated with the read.
Do not rely on a later store inspection to reconstruct a transient read's view;
record that view at the mismatch when possible.

The script also uses `set -u`, writes the workload return code into `a.txt`, then
continues to cleanup and ends with `echo DONE`. It can therefore return shell
success after the workload fails. Preserve the workload exit status, report
cleanup failures separately, and return failure for a failed workload. A DONE
file proves termination, not PASS. Audit how the workload detects and propagates
mismatches before using shell status as the campaign verdict.

### What closes the investigation

Five alternating runs per build are a reasonable minimum investigation sample,
not proof that an intermittent mismatch has been ruled out. Match workload,
packing age, concurrency, fixture setup and deadlines; record that packing/repack
actually occurred. Use clean source archives and exact daemon/client identities.
Add a targeted small-packed-file → growth → concurrent modify/read case, and
verify the test oracle's synchronization/version rules as well as the filesystem.
Do not mask errors with retries or extend timeouts to manufacture PASS.

Reproduction on the base build establishes that a baseline issue exists; it does
not prove the branch cannot trigger or worsen it. Establish the causal mechanism,
add a regression for it, fix it or demonstrate a concrete oracle defect, and
repeat branch acceptance. If it cannot be reproduced and original evidence is
unrecoverable, retain OPEN/UNRESOLVED status and the exact limited clean-run
results; do not silently convert that history into PASS.

Also add the missing tombstone/work-budget scan-stop test: stop/resume must make
bounded progress without skipping actionable keys, including term change and
wraparound. Callback-limit coverage alone is not this gate.

**Recommendation:** fix the evidence harness, investigate the read mismatch with
preserved per-read state, then qualify the final branch. No new user-facing A/B
mode is needed: compare separate private builds, then land one qualified behavior.
This is part of alpha 1.0's packing correctness and evidence gates, not a reason
to defer correctness in exchange for lower CPU consumption. No code was changed
by this assessment; all additional tests described here remain NOTRUN.

## Reviewer follow-up to the assessment — October 22

**Status: merged into devel at `842a014b3` by user decision. The read
mismatch stays OPEN, but evidence now points away from the branch.**

### Correction to my review: the mismatched file was never packed

`w125_concurrent.py` content is a pure function of (name, version), so every
version of the file can be recomputed with the script's own `body`,
`op_of`, `apply_op` and `contents`. The workload names the file
`w2/d049/f004799`, relative to the mount root.

| Version | Operation | Size (bytes) |
| --- | --- | ---: |
| v0 | create | 3,290,288 |
| v1 | append | 3,299,550 |
| v2 | append | 3,305,263 |
| v3 | rewrite | 3,629,464 |
| v4 | extend | 3,631,970 |
| v5 | overwrite | 3,631,970 |
| v6 | extend | 3,633,734 |

- **Never packable.** No version is near the 16 KiB packing threshold, so
  the file never had a packed base.
- **No changed path reads it.** The client never read it through
  `efs_pack_read`. The packer never selected it, and `pk_read_bases` never
  repacked it. My "no changed code path does not hold" argument (that it
  might have been packed while small) is wrong for this file. The branch
  doc's statement stands.
- **What remains.** The branch can only have influenced this read through
  timing, for example less packer CPU or different scheduling. That does not
  make the branch a cause, and it does not clear it either.

### A concrete hypothesis

- **The sequence.** v5 overwrites up to 4,000 bytes in the middle at the same
  size, then v6 extends with zeros (`truncate` up).
- **Why size alone can't catch it.** v4 and v5 have the same size, so a read
  that misses v5's overwrite but sees v6's extension has exactly v6's size
  and wrong bytes. That matches "correct size, wrong bytes".
- **Candidate.** A stale chunk or a lost overwrite on the client read path
  after truncate-up. That would be a devel bug independent of packing.
- **Not established.** The returned bytes are lost, so this cannot be
  confirmed for the original event.

### Assessment points

- **Agreed:**
  - "code correct" should read "no defect found in the reviewed mechanisms;
    focused tests PASS";
  - the implementation of `efs_pack_read` did change;
  - a matching digest does not prove the reference was the right version;
  - five clean reruns cannot explain the original event;
  - a tombstone/work-budget stop test for the abandon scan is still missing
    (NOTRUN).
- **Lifetime (checked):** image and payload pointers do not outlive their
  buffers on the merged code. `pk_base_run` copies each record into `f->data`
  before freeing its image. Compaction keeps `images[si]` until the copied
  records are used. Read-through views never escape their function.
- **Harness defects confirmed and fixed (this commit):**
  - `scripts/w125_modify_repro.sh` deleted the previous run's store, and
    returned success after a failed workload.
  - It now uses a fresh directory for each run, refuses to reuse one, keeps
    the store of any failing run, and deletes only passing stores. It
    records the arguments and the sha256 of `efsd`/`efs-fuse`, and exits with
    the workload's status (2 when the cluster does not start, 3 when only the
    stop fails).
  - `scripts/w125_concurrent.py --mismatch-dir` saves the exact bytes of each
    mismatching local read (first 20), so `w125_modify_inspect.py DIR/N NAME`
    can show which version each 64 KiB block holds. This records the state of
    the transient read itself.
  - Checked only by `bash -n` and a 4 s local run of the workload (0
    mismatches). Not yet run on a cluster.

### Remaining gates (all NOTRUN)

1. Run the repro at least 5 times each on devel `842a014b3` and on the
   pre-merge devel, alternating. Use the fixed script and keep any failing
   store and mismatch dump. If a dump shows a v4 block where v5 overwrote,
   file a devel client-read bug (truncate-up after overwrite).
2. Add a targeted regression: on a large file, overwrite in the middle at the
   same size, then truncate up, then read back immediately, under concurrent
   modifiers.
3. Add the abandon-scan work-budget stop and resume test, including term
   change and wraparound.


## Independent assessment of `a1ceff4cb` — October 22

**Verdict: correction accepted for completed versions; read mismatch remains
OPEN. Harness improvements are useful but evidence-preservation and shutdown
checks remain incomplete.** Reviewed `a1ceff4cb` and its unchanged scripts on
`d94ca85bf`; this is a source review and deterministic fixture reconstruction,
not cluster qualification. No daemon, store, workload or deployment was changed.

### What the reconstruction establishes

I independently evaluated the workload's pure `body`, `op_of`, `apply_op` and
`contents` functions for `w2/d049/f004799`, versions 0–6. All seven lengths
match the table above. The v5 overwrite contains 2,601 bytes; v6 adds 1,764
zero bytes. This disproves my earlier suggestion that a **completed version**
of this particular file started small and was subsequently grown out of packing.
The overwrite-followed-by-extension case is a good focused regression target.
It is not a diagnosis: the original returned bytes and mapping were lost.

Distinguish this reconstruction from an observed mapping history. Rewrite uses
`truncate(0)` before writing, so completed sizes alone do not prove that the
file never transiently became small. The intended packer guards reject zero or
over-threshold sizes, require quiet age, and reject an open lease
(`src/server/packer.c`, candidate selection). With those invariants intact,
this file should never be selected. There is no retained evidence establishing
a violation, and no reason to assert one. Prefer **“no completed version was
packable; no demonstrated packed-read involvement”** to an unconditional
historical claim. Timing or shared-state interactions remain possible; this
review neither attributes the mismatch to packing nor clears the merged build.

### Remaining actionable harness findings

1. **Output reuse can still erase evidence.** `w125_modify_repro.sh` rejects an
   existing `RUN`, but accepts an existing `OUT` with `mkdir -p`. It truncates
   `run.txt`, `start.txt`, `a.txt`, `stop.txt` and `done`; mismatch numbering
   restarts at 1. Require a fresh output directory too, and check directory
   creation and identity recording before launching. Test two invocations
   using the same output directory: the second must refuse without changing
   the first run's artifacts.
2. **A zero stop status does not certify safe cleanup.** The invoked
   `w125_p0_cluster.sh stop` ignores the client-stop result, sends TERM, waits
   three seconds, sends KILL, then ends with successful `echo stopped`.
   Therefore the new wrapper's `stop_rc` check cannot detect ordinary unmount
   or shutdown failures, and may delete the store after an unverified stop.
   Check mount absence and termination of the owned processes before deletion;
   distinguish forced termination from orderly shutdown and retain its store
   and logs. Until then this harness cannot qualify orderly restart. Add
   injected unmount-failure and delayed-exit tests before relying on PASS cleanup.
3. **Tie saved bytes to their expected state.** `save_mismatch` records a name
   and a counter, but no expected version, hashes or timestamp. Error reporting
   and dump-number allocation use separate lock acquisitions; concurrent reads
   need not appear in matching order, and the error list is separately capped.
   Save a sidecar with each dump containing expected version, expected/actual
   lengths and hashes, source/build identity and read time, captured while the
   existing per-file lock still protects the version. Report artifact-write
   failures explicitly. Preserve final-verification mismatches as well.

These are focused follow-ups, not a request for additional runtime flags or
legacy-format support. Failure-path tests can use a stub cluster helper and
synthetic mismatches; another expensive cluster run is not needed to establish
these harness properties. Those tests, the targeted overwrite/extend regression,
alternating fresh-cluster qualification and the abandon-scan work-budget gate
remain **NOTRUN by this review**. The user-authorized merge remains in place;
alpha qualification must continue to report the unresolved mismatch separately
from passing unit tests.

## Reviewer response to the `a1ceff4cb` assessment — October 22

**Agree with all three findings and the corrected wording.** I checked them
against the scripts on devel `d53bfd0db`:
- **Output reuse:** `w125_modify_repro.sh` refuses an existing `RUN` but runs
  `mkdir -p "$OUT"` and then overwrites `run.txt`, `a.txt`, `stop.txt` and
  `done`.
- **Unchecked stop:** `w125_p0_cluster.sh stop` discards the client-stop
  status, sends TERM, waits 3 s, sends KILL and prints `stopped`. A run that
  passes therefore never tests an orderly stop, and the store kept from a
  failed run may come from a SIGKILLed daemon. Record that in the run's
  `done` until the stop is checked.
- **Mismatch context:** `save_mismatch` takes its counter under `lock`, but
  the dump carries only the name and the bytes.

I retract my earlier "the file may have been packed while small and then
grown". The assessment's narrower claim stands: no completed version was
packable, and nothing shows the packed-read path was involved. That leaves
the mismatch where the assessment puts it: OPEN, unattributed.

Two more points for the same harness work:
1. **The `--remote` reader path saves nothing.** A cross-client read that
   matches no version is retried up to 3 times, 2 s apart, and counted as
   `transient` unless it never settles. That tolerance is legitimate for torn
   reads during another client's in-place overwrite. But the first
   non-matching bytes are never saved, and a read that settles leaves no
   trace beyond a counter. Save the first non-matching read, with the same
   sidecar as finding 3, and report whether it settled and after how long. A
   failure path that only bumps a counter is the masking this review warns
   against. The single-client modify repro does not use `--remote`, so this
   does not affect the open mismatch.
2. **Use `edelete`, not `rm -rf`.** The PASS cleanup in
   `w125_modify_repro.sh` should use `~/bin/edelete --force --delete "$RUN"`
   on ORCD nodes, and only after the checked stop of finding 2.

No code changed in this response. The harness fixes and the reruns stay
NOTRUN.
