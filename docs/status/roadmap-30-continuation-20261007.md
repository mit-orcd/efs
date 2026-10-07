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
