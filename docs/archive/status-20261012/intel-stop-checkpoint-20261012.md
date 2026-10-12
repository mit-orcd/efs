# Intel stop checkpoint — 2026-10-12

The human explicitly answered the pending clarification: **Document, commit, and stop**. No additional tests, implementations, deployments, promotion, or launches are authorized by this checkpoint. Subsequent coordinator evidence-work requests were informed of this direct stop.

## Final qualification state

Common candidate `03bd89fb7de56c2c6af44ab3bd26c54caf4b8274`: build and 13 focused checks passed. The broad seven-client TCP/direct quiet gate FAILED after 146 seconds, including concurrent/directory/query timeouts, fsync and basic-write EIO, basic-seek EBUSY, and a refused graceful client stop. Peer, cold-hash, and physical gates were NOT RUN after that failure. The candidate is not qualified for production. Full TCP/buffered acceptance remains unfinished.

A previously completed reduced-load deep-directory diagnostic ran the original `dir_deep_nesting` case on seven clients with the unchanged 30-second deadline: all passed in 13.447–16.554 seconds, and all seven graceful stops passed. Controller cleanup confirmed no remaining private processes or mounts and all 23,318 source inputs matched. This narrower diagnostic does not replace broad-suite acceptance. Its 50 raw evidence files were collected and verified against SHA-256 and size manifests; the external observer helpers are retained alongside them in `results/measure/20261012-intel-common03/deep-diagnostic/`. The Python call journal does not count all kernel/FUSE/RPC operations; scandir records constructor timing only.

The saved broad-gate FUSE logs map fsync/flush-meta EIO to `EFS_ERR_NOT_PRIMARY` (-15) propagation. The exact underlying RPC stage and acknowledged-byte status remain unresolved. The seek EBUSY stage also remains unresolved. No data-loss conclusion is supported. Investigate load interaction, receiver pump/durability delay, and flush ownership without masking refusals or extending deadlines.

## Stop state

All Intel private diagnostic controllers have completed and cleaned up. Production daemon identities remain intel1 PIDs 5945/5953 and intel2 PIDs 10086/10094, still TCP/direct with 400G per daemon. No production redeployment or store erasure was performed in this qualification phase. Existing production mount processes are checked separately at checkpoint time. The historical million-file jobs completed; they were not restarted. Previous failed evidence and native checks remain committed.

Implementation commits are retained separately: leader-hint correction `2bf7e257`, extractor adapter `def46109`, and bounded FIFO drain `6b097303` (central cherry-pick `03bd89fb`). No merge into devel or release promotion is claimed. Resume only on a new direct human instruction.
