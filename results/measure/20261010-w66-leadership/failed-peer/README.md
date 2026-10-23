# Append OPEN refusal exposed by full gate

Candidate private AMD direct/TCP fixture `gc-direct-fuai_vi1`: core POSIX
212/212, peer 63/64. `peer_concurrent_append` failed with EBUSY on peer OPEN.
`peer-fuse.log` recorded unresolved OPEN sequence 88, inode 5096, flags 1,
rc -13 at 12:00:31.644Z. The other writer subsequently published 2400 bytes.
No balance handoff occurred at that boundary; the last was at 11:59:51.780Z.
This does not establish that balancing introduced the defect.

Production OPEN issued its retained edge once and returned unknown BUSY.
The OPEN guard checks transaction ownership (inode and close-step keys), which
can contend with append/close. The corrected path retries unknown BUSY with
the exact identity within the earlier caller deadline or a 30-second ceiling.
Terminal verdicts and other errors remain errors. Timeout retains ownership
for cleanup; no fresh identity bypasses uncertain state.

The negative control keeps the helper present but substitutes the prior
one-shot production call. It fails the transient-BUSY assertion, rather than
failing compilation. The fixed extraction tests pass that case and expiration,
retained ownership and original-deadline restoration. Earlier open/close
serialization coverage still executes the production helper.

Raw complete original fixture logs remain on AMD in
`/data1/efs/evidence/lease-retention-fixtures/gc-direct-fuai_vi1`; this directory retains
the test transcript and failing TSV. Installed service was restored.

**Raw evidence note (2026-10-23):** the bulk data in `lease-retention-fixtures` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
