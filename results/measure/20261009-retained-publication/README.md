# Retained typed publication writeback, Oct 9

AMD private four-daemon fixture, direct-I/O/TCP. Installed stores preserved and
installed service restored after each run. Software RXE/hardware power-loss are
not acceptance claims.

Implemented: exact post-snapshot range ownership; pre-PUT snapshot race refusal;
cold default-export lane bootstrap before any PUT; budgeted immutable publication
queue; status-first recovery; atomic cache result consumption; lost retirement
ACK ownership independent of cache lifetime; first typed range admission.

Initial live gate (gc-direct-38oskc3f) passes real cache bytes, intentionally lost
publication/retirement replies, peer update outside newer4KiB, exact cold sparse
bytes, physical deletion, offline fourth-member/rejoin/restart and late-PUT GC
fencing. Complete code regressions pass; Linux build warning-free. See live.log,
regressions.log, build.log and typed-writeback-wire.txt.

Initial broader rerun stopped on a natural-exit process-discovery race before
POSIX acceptance. Linux may clear cmdline before Z; PF_EXITING distinguishes that
confirmed departure from a live ambiguous command. Ownership refusal tests pass.
The successful rerun and stronger cold replay gates are recorded below.

Production upload remains unticketed and default FUSE flushing remains legacy.
No D25 activation or full package closure is claimed.

Broader candidate gc-direct-kar3f_ch exits0: core212/212, peer64/64,800 exact
cold append records, O_SYNC/O_DSYNC, server deletion counters and physical GC,
offline-member/rejoin/late-PUT fencing pass. See full-live.log and replay-wire.txt.

Strengthened cold gate gc-direct-52ytjj7i exposed post-truncate admission BUSY:
legacy lane fence advanced the stamp but not typed authority/history. This is
a code defect, not cold-start timing. Atomic cross-group fence update passes
gc-direct-7k8lrywc cold retained replay and real truncate clipping. Same-group
interop extension passes gc-direct-rqo6bsa1 (final-full-live.log): core212/212,
peer64/64,800 cold append records, sync writes and GC/restart. Both
typed-writeback-*-cold-replay.txt files prove retired replay and real truncate
clipping. Atomic metadata fault and history-capacity regressions pass; the hard
truncate batch bound remains unchanged.

The overlay API requires fresh complete FileID/lane/base authority. Legacy read
helpers refuse typed buffers rather than return unfenced dirty bytes. Failed
views leave destination unchanged; a complete fence may release fully clipped
bytes, but never an unresolved publication identity. Fragmented post-snapshot
write33 is refused without copying bytes or mutating ownership.

Final queue-reserved candidate gc-direct-r09t3v8f passes both real typed streams,
lost ACKs, cold retired floors, truncate clipping, ownership release and complete
GC/restart cleanup. Complete code regressions pass (final-regressions.log); Linux
build warning-free (final-build.log). final-driver-cases.log also proves that an
unconsumed RETIRED floor cannot authorize byte release; NOT_FOUND/PENDING replay
uses the unchanged retained request. amd-source-sha256.txt matches the committed
production sources.

Focused gc-direct-b9kgkazc kills the actual lane leader for each stream, queries
retired requests through the replacement, then repeats cold and real truncate
proofs. Exact state checks remain mandatory; only NET/BUSY/NOT_PRIMARY/NO_QUORUM
receive bounded election retries. Initial immediate-query assertions failed
during election; that fixture restored AMD. Final leader gate exits0 and restores all four installed AMD daemons and both
mounts (final-leader-live.log and restored-service.txt); reservation released.

Remaining production work: reserve/capture PUT admission identity and membership
before upload; validate admission at receive/store with durable revocation
fences and all-member deletion ACKs; integrate retained queues into the default
FUSE flush lifecycle and revocation reconciliation; retain other ambiguous
namespace requests with authoritative routed results. Legacy W88 eviction/re-PUT
acceptance and final activation gates still remain. D25 is staged.

Final checkpoint: code commits47394c4c,2afaa8fb,9a6ca38e,643d4404,2002a028;
fault-test commit e9f6f2cd; actual leader-death gate commit b5130037. All owned
fixtures exited/stopped; installed978caaf7 direct/TCP service restored with4/4
identical nodes healthy and both mounts serving. No production activation,
merge, push or tag. No efs-bench sources/harness changed.
