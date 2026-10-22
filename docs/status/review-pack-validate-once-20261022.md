# Review of `claude/pack-validate-once` — 2026-10-22

**Verdict: code correct, tests PASS. Not ready to merge until the
modify-under-load read mismatch is reproduced or ruled out (open item
below).** Subject: commits `207dde6b4`, `660c6621c`, `745af27f5`, `e223b88d5`,
`7cef019b4` and `1df84466d` on `claude/pack-validate-once`, based on devel
`dd43b4015`. They fix findings 1 and 2 of
[the ORCD tiny-file profile](orcd-tiny-profile-20261022.md). Nothing is
merged or pushed.

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
