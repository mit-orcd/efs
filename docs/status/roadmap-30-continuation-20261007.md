# Requested 30-round continuation — Oct 7, 2026

Baseline `1a3a1143`. Test every round on NUC; preserve concurrent benchmark
work and use isolated source snapshots for deployments. A round is a concrete
implementation/investigation with its own validation, not an arbitrary tiny
commit. Only completed rounds are counted below. Remaining rounds continue in
priority order; stop for a required design decision rather than guessing.

## Round 1 — W82 stale namespace lookup gate

Extended the real-daemon peer-partition fixture to rename a file on the majority,
then reject LOOKUP of its stale name at the isolated former leader. GETATTR and
LOOKUP must return explicit BUSY/NOT_PRIMARY; transport failure or daemon death
cannot satisfy the test. Confirm the old daemon still reports LEADER, then
verify the renamed entry after healing.

NUC private daemon gate passes, including eight concurrent GETATTRs and three
LOOKUPs. Evidence `/private/tmp/efs-roadmap-round03-partition.log`.
Transaction/session/publication view and configuration-change gates remain.

## Round 2 — W89 measurement serialization

Replaced packed KV metrics with validated twelve-column TSV serialization and
strict reduction. Missing observations remain NA rather than becoming zero;
malformed rows stop acceptance, and counter peaks survive reset.
NUC: four schema/regression tests pass; driver bash syntax passes.
Evidence `/private/tmp/efs-roadmap-round04-w89.log`. A new pressure run remains
owed under W23; this round fixes the measurement machinery only.

## Round 3 — observable server GC effects after POSIX deletion

The live FUSE/GC harness waits for passes while hardlink/open-unlinked protection
is tested. After last close it requires physical target fragments absent, passes
advanced, removed-fragment/payload-byte counters increased, and usage decreased.
After broad POSIX deletion it also requires increased pass/removal counters;
final restart still requires empty physical inventory, zero data usage and
retired queues. Recorded JSON checkpoints live in
[GC effects](../../results/measure/20261007-gc-effects/direct-posix.json).

NUC buffered and direct private four-node tests pass: single POSIX 216/0/1 skip,
peer 64/64, failure/restart/offline-member protection, zero inventory/quota and
late-PUT fencing. The direct run exercised the new broad-POSIX counter assertion;
both modes exercised the last-close assertion. Prior W87 failure remains retained;
one successful broad repeat does not establish its cause or close W87.
The NUC devops driver adds `--gc` (private stores, no production data deletion).

## Round 4 — W75 fail closed without recorded integrity evidence

Data GET rejects missing/truncated checksum evidence instead of manufacturing a
matching digest from untrusted bytes. Valid objects and existing mismatch checks
remain covered. NUC real-RPC corruption tests pass for buffered/direct and one/two
roots. The old production binary fails the same missing-digest assertion,
confirming regression sensitivity. Logs `/private/tmp/efs-roadmap-w75-integrity.log`
and `/private/tmp/efs-roadmap-w75-baseline.log`. Identity-bound format remains open.

## Round 5 — W75 comprehensive optional client payload verification

The ordinary parallel GET path now honors EFS_READ_VERIFY, uses the same verifier
as fallback GET, hashes bytes even when the returned digest is the known-zero
value, and initializes the option once without a data race. NUC production-code
reply tests cover valid/corrupt zero/nonzero packets; all sixteen real-server
corruption combinations still pass. Log `/private/tmp/efs-roadmap-w75-verify.log`.
This verifies payload hashes; it does not claim identity-bound integrity or repair.

## Round 6 — W75 validate PUT evidence before mutation

Data PUT independently hashes the received payload and rejects a mismatching
caller digest before storage/quota mutation. NUC buffered/direct, one/two-root
RPC tests reject wrong-digest retries and verify that the existing valid object
is unchanged; the GET corruption matrix and client verification tests pass.
Log `/private/tmp/efs-roadmap-w75-put.log`. Adds one hash per data PUT; identity-
bound format and metadata-table integrity remain separate work.
