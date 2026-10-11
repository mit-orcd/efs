# W91/W92 command options and W90 expiry test status — Oct 8

Branch: `devel-find-stats`. This change implements the user's accepted display,
sorting and type/size/modification-time filter proposal. Usage and exact option
semantics: [command guide](../using/efs-commands.md). It does not implement W90.

## Implementation

`ls`/`find` gain long output, numeric ownership, readable sizes, UTC/relative/full
modification dates, name/mtime/size sorting, reverse and relative paths. `ls`
adds directories-first. `find` adds type, inclusive logical-size bounds, strict
absolute timestamp bounds and a relative recent-modification interval. Existing
name matching, JSON, NUL-safe output, scope and source-read behavior remain.

The catalog already holds folded authoritative snapshot stats. Query entries
now include `uid`, `gid` and `mtime_ns`; there is no per-entry stat RPC. A v3
request extends the existing 808-byte v1/v2 layout to 848 bytes with canonical,
bounded filter fields. Existing request layouts and reply headers remain
unchanged. The new read-only filtered path uses canonical unpadded base64url
for its filename term, fitting all four uint64 bounds plus the existing 120-byte
literal within a 240-byte component. No shell evaluation or writable query file
is introduced. The server echoes applied filters; clients reject missing or
mismatched echoes. Upgrade server and FUSE together for these options: unknown
versions or missing fields fail explicitly rather than silently dropping filters.

Filtering reads the immutable catalog and checks ancestry/permissions/deadlines
even for excluded name candidates. This prevents predicates becoming a probe
of inaccessible descendant metadata. Matching entries retain current binding
and final directory/layout checks. Candidate visits are charged even when a
filter excludes everything; filtering precedes output-size accounting. Results
are complete within the declared query or fail explicitly. Sorting is bounded
by the existing complete-result limit and happens on the client after validating
every path and scalar. JSON retains raw integers and snapshot identity.

## Validation

Local command/codec tests: 22 passing. These cover C/Python path round trips,
maximum raw-byte patterns, canonical encoding refusal, size and nanosecond date
boundaries, filter combinations, invalid ranges, missing/mismatched filter
echoes, malformed metadata with no partial output, sort directions/ties,
directories-first, safe terminal rendering and unchanged source-only reads.

The first NUC candidate `ae91a7f7-command-options` passed all native units and
full private gates: POSIX 216/0/1 (22.7 s), peer 64/64 (12.3 s), raw v1/v2/v3,
options through both mounts, permissions, immutable results, loaded publication,
publisher loss/rejoin, noexec, cold opt-out and on/off/on restart. During review,
excluded entries were moved behind ancestor authorization/deadline checking;
a new gate requires permission denial even when a size predicate would exclude
the inaccessible entry. The final candidate and validation are recorded below.

Evidence directory:
[command-options measurements](../../results/measure/20261008-efs-command-options).
The production stores and historical generator trees are preserved; fixture
stores and rollback binaries remain on NUC.

## Expiry testing

**No positive expiry lifecycle acceptance exists for this branch.** W90's D33
policy is accepted, but deadline metadata/index, renewal, clock-safe expiry worker
and automatic namespace deletion are not implemented or activated here. The
independent main checkout also records W90 as implementation/gates open at this
review; no main-checkout files were changed.

What is tested:

- Command tests reject unsupported expiry options rather than silently ignoring
  them, without producing result output.
- Live help/JSON report `expiry: unavailable`; entry expiry is null, not "never".
- The live option gate rejects both `--expires-within` and `--expires-before`.
- Existing traversal gates exclude synthetic `.efs` entries from normal READDIR.
  This is an interface test, not an expiry scan/deletion test.

What has **not** been exercised as expiry behavior:

- Directory TTL inheritance, persistent deadlines and authorized renewal/clearing.
- Renewal versus deletion, rename/path reuse, and expiry-specific hard-link rules.
- Automatic removal at eligibility and retained access through open descriptors.
- Clock uncertainty/jumps, expiry worker crash/restart or leader changes.
- Concurrent publication, offline storage nodes, and physical reclamation caused
  by expiry rather than ordinary explicit unlink/GC.

Ordinary POSIX, peer and GC passes do not establish those guarantees. The
required future suite must implement D33's lifecycle and test namespace removal
and physical reclamation separately. No file-age or mtime-based deletion was
introduced by the new find predicates. W90 remains open; the broader W91/W92
400 GiB, platform, fault-matrix and remote export-policy acceptance also remains.


## Final build acceptance

Implementation commit: `c50f4b0c`. Final native candidate:
`ae91a7f7-command-options-2` (the embedded candidate identity is not a literal
commit SHA; the source progression and binary hashes are retained).

Metadata/catalog units and all 22 command/codec tests pass. The final clean
fixture passed POSIX **216 passed / 0 failed / 1 skipped (21.6 s)** and peer
**64/64 (12.8 s)**. Both mounts pass snapshot timestamp/size/owner comparisons
against independent `lstat`, exact nanosecond and inclusive size boundaries,
all entry types, ordering/reverse/ties, UTC/full/relative formatting, raw/control
names and NUL-safe relative output. Filtered output fits a cap which explicitly
refuses the unfiltered query; tiny visit budgets still refuse even a filter
which excludes every result. Protected descendants deny access even when the
size predicate would exclude them. Old filtered descriptors retain their bytes
through permission changes and peer mutations; new queries converge.

Full v1/v2/v3 validation/limits, loaded publication with two simultaneous query
readers, publisher loss/rejoin, noexec, cold opt-out, all-three-version refusal
while off, preserved authorized descriptors, and on/off/on rebuild passed.
Loaded payload oracle: 4,300 files / 1,100,800 bytes. Restart oracle before the
off-phase mutations: 4,329 files / 14 directories / 1,103,054 bytes.

The first production deployment attempt stopped at the graceful unmount check:
the generator had independently changed from `tiny-command-perf-final9` with
eight workers to `tiny-command-perf-final10` with two, so the previous targeted
stop did not match. The guard resumed mutation admission and changed no binaries.
After verifying the running binaries were still round 9, deployment was adjusted
to stop the current generator and preserve its two-worker setting. No force
discard or store reset was used; the initial refusal log is retained.


## Production result

The graceful retry completed. All four NUC daemons and both FUSE mounts run
`ae91a7f7-command-options-2`; running `/proc/PID/exe` hashes match the installed
candidate. Both mounts are available over TCP/buffered I/O with commands enabled
and the existing 1,024 MiB catalog cap. The two-worker generator now writes
`/data1/efs/mnt/tiny-command-options`; previous trees remain intact.

Production filtered find returns exactly 25,725 paths in the frozen `tiny-files4`
subtree. The independent oracle still matches 25,725 files and 99 directories,
with unchanged sorted raw-path digest. Modification-time sorting, exact
size/type filters, long/readable-size/full/relative date output and help pass.
Thirteen sampled entries exactly match `lstat` mode, numeric ownership, size
and nanosecond mtime. No test interprets modification time as expiry.

Production output examples, timings, oracle, runtime/binary identities and the
successful/initially refused deployment logs are retained in `remote/final/`.
The final private fixture's logs and raw POSIX/peer TSVs are in `final-fixture/`.
These options do not close W90 or the remaining W91/W92 scale/platform/policy
acceptance items.
