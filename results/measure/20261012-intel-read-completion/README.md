# Intel ReadIndex completion investigation (resumed 2026-10-12)

Source foundation: frozen release `28b2089b`. Historical stop evidence remains in commits `6acd3bdd` and `1ef02793`. Human reply in AMD coordinating chat explicitly resumed fixes and affected tests; the original 24 unrun matrix scenarios are not automatically restarted.

The deterministic ordering regression shows the original host coverage predicate can lose an eligible completed round when a newer pending round replaces the published state. Private candidate `77fabfbc` attempted bounded retention but changed raw `read_done` semantics shared by the committed-publication proof. That candidate is rejected: raw latest-round quorum must not be manufactured from an older completed proof. Its quiet run recorded early EBUSY/EIO and failed graceful extra-client teardown at148.957s. Peer/cold/physical oracles were NOTRUN. The fixture already exited before a requested targeted TERM, so no termination signal was sent. Production was never deployed or replaced.

Corrective `28dfe54e` separates raw current-round quorum from retained admitted completion and tests actual coherent host publisher/reader plus the exact publication predicate under100,000 concurrent publishes. `c10a0678` adds the actual Raft parked-apply regression. Focused Linux build and11 checks PASS, but corrected quiet7/jobs16 acceptance still FAIL with early EBUSY/EIO and graceful teardown failure. This candidate is also unaccepted; fixing the raw-field contract did not fix the workload.

A fresh frozen28 quiet control on the same ports34510–13, same7clients/jobs16/4G, original30s/75s/180s budgets and original assertions reproduced the original POSIX timeout pattern. It failed graceful extra-client teardown at125.254s; peer/cold/physical oracles were NOTRUN. A subsequent diagnostic isolates core record/invalidation from host retained-coverage use by restoring only the original host coverage predicate. It is diagnostic evidence, not fixed-source acceptance.

Both completed fixtures retired safely and retained exact input manifests (21739 rejected source,21738 frozen control,21740 corrected source). Raw stores remain on Intel; committed evidence excludes node store contents. SHA manifests verify fetched logs. Between windows all original production process identities, binary hashes, versions and mounts match baseline; all256 saved samples from each host match (512 checks). Full2M inventory is not claimed.

No shared release move, integration, production deployment, push or tag occurred. AMD independently reviews logical changes; no implementation candidate is accepted yet. Later evidence and final preservation proof will be appended after current diagnostic work completes.


## Final checkpoint — 2026-10-12 10:56 UTC

The core-only diagnostic control failed in125.447s with the original timeout pattern. The bounded first-cause traced run completed naturally in406.7s: all seven concurrent POSIX clients exited1; the peer suite passed67/67 and acknowledged cold hashes passed, but packed physical fragments were not reclaimed within the original180s limit. Tracing changes are private diagnostic overlays, not an accepted fix or quiet acceptance. Candidate c10a0678 remains unaccepted. No further tests were launched.

The controller finished at10:56:12 UTC, reporting no private processes or mounts and21740/21740 input hashes matching. All56 fetched diagnostic evidence files were SHA256/size verified. Stores remain on Intel. Final production verification on both hosts exactly matches the preceding process identities, installed/live binary hashes, versions and mounts; all512 immutable payload samples match. Both hosts can connect to all four production17432/17433 endpoints, and private34510–34513 ports are idle.

The current chat received text requesting document/commit/stop, while the coordinating chat reported the verified resume instruction remained current. Automatic approval review rejected a message describing that text as a new human stop because it judged the text assistant-style/untrusted. A direct clarification is pending. This checkpoint preserves the existing bounded run and makes no further launches or claim of acceptance; no production deployment, push, tag or shared release change occurred.
