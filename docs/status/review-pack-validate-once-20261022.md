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
