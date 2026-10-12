# Mac retained namespace audit — 2026-10-12

Scope: manager-authorized isolated read-only investigation on devel-mac. No installed-store writes, deployment, repair, or acceptance inheritance.

## Implemented and tested

`tests/retained_namespace_audit.h` reads only a caller-pinned local LSM snapshot. Its KV adapter has GET only; no mutation callbacks. A bounded global scan counts all record families, resolves layout-selected dentry copies, checks backlink generation/parent, child generation/liveness/parent, directory nlink against child directories, and marks any pending INTENT/GUARD/REDUCE or splitting layout inconclusive. Cancellation and budget exhaustion leave local coverage incomplete. Snapshot lineage and transaction revision qualify local evidence only.

`tests/test_retained_namespace_audit.c` exercises the actual frozen b239dc7de104ff8437e49d9333a529790601464c real-LSM library on Mac efs1 in `/tmp/efs-mac-readonly-audit-e2KDby`. Normal and fully instrumented ASan/UBSan executions PASS; compiler uses Wall/Wextra/Werror. Fixtures cover healthy children/nlink, direct rename replacement refusal, rmdir and rebinding, immutable old snapshot, cancellation/budget failure, cold reopen, a deliberately seeded live orphan, pending shared guard and aborted-guard cold recovery. Removing the missing-backlink check makes the sanitizer regression fail at the seeded-orphan assertion. These are isolated fixtures, not live acceptance gates.

Direct rmdir deletes the inode row; it does not retain an nlink-zero row. Neither ordinary direct apply sequence nor cold recovery reproduced a spontaneous live orphan. The seeded negative fixture restores a saved live row only in the private test store. It provides no evidence that a production writer used that sequence.

## Evidence identity

- Source archive SHA256: `804d8bf6f7e8e646bfe1a684174545b43b38232ebb2d40653ecaca9b9bd8ed64` (git archive of b239 Makefile/include/src/deps).
- Audit header SHA256: `57be2aa5973af8d1f6a1342f7931f3f6698c7f072b234fbd941b6ddbb81fe19f`.
- Fixture SHA256: `748f895d5777124d74d17289d6402a4f5c856c134393542dc1111f3410cba24a`.
- Fully sanitized executable SHA256: `6f6afdd52ca9fbccba656e943a2b2da693835120ecd9074cbfe99fda9360cef9`.
- Private build logs: `build.log`, `sanitizer-build.log`; negative control: `mutant.log` in that private directory. Installed efsd remains active; no installed binaries/stores were changed.

## Explicitly inconclusive and still open

This is a local snapshot audit primitive, not a distributed audit certificate or installed-store command. Global scan means all keys in that snapshot, not all authoritative replicas. Ownership remains inconclusive even when session/lease/GC/ticket counts are zero. Repair eligibility and distributed coherence are always false. Pending decision records and record counts do not prove client revocation or completed reclamation.

For installed inode 4091, a coherent audit still needs a read-only server-side pin taken under the existing authoritative cut protocol, all owning-group applied vectors and replica coverage, then identity-aware session/lease/PUT-ticket/orphan/reap/GC decoding. Concurrent prepared transactions must be resolved or leave the result inconclusive. Repeated point RPCs cannot substitute for this cut. Provenance must include build/HELLO identities, store identities, authority/applied vectors, capture interval and limits. Any resource error, missing group, unsettled owner, unknown schema or inconsistent cut forbids repair.

A repair proposal needs the actual writer cause, generation/parent/backlink preconditions, transaction and session fence proof, complete children coverage, replay identity, crash recovery and compatible-reader rollout proof. No such proof exists yet; no guarded repair is implemented or authorized. Retained 4091 and the original-budget packing acceptance failures remain open independently of these passing fixtures.

## Partial transaction discriminator

Follow-up actual real-LSM normal and fully ASan/UBSan tests pass for a lower-level replacement-shaped transaction: prepare source dentry DEL, destination dentry PUT, victim inode DEL and parent nlink/nents delta; commit decision, resolve the parent/dentry participant only, then cold reopen. The old victim remains nlink2 with no authoritative backlink, but its inode DEL intent remains pending. The audit reports both inconsistent binding and inconclusive ownership/transaction state. Resolving the victim participant removes it and leaves the source inode with exactly one coherent backlink.

This demonstrates why the observed graph shape alone cannot prove corruption eligible for repair. It is a deliberately interrupted transaction-layer model, not the full production host rename path (authorization, DSEQ guards, source pver and replay admissions are not exercised), and not evidence that installed inode 4091 came from this sequence. A coherent installed audit must inspect the relevant intent/coordinator decision before distinguishing unresolved commit from an abandoned live row.

Updated fixture SHA256 `6086855e35c835bff350cfd22ec3b836ef88eba6a5eca03bf191b0c2b0fff0fb`; normal executable `f6a4a8b169fe31d9b778e0609189737e2294c2435f034d93965192ad814aa929`; fully sanitized executable `7102a232b4e316627ef1c759ef25d34a27ce69b0fe88920b024da50ba58f59b7`. Same exact b239 source archive and private Mac directory. Installed service remains unchanged.
