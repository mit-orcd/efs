# W93 / W53 production mixed-load gate

AMD one host, four private daemons, direct-I/O/TCP. Eight tiny workers create
and close 512 512-byte files while four writers repeatedly overwrite bounded
16-MiB files, fsyncing every four MiB. All final tiny and large bytes are
verified after abrupt private-daemon restart, then deleted; GC queues and
physical inventory drain. Installed service restored after each fixture.

Baseline is 0ed74ef8 (W96 fixed, no cross-request W93 batching). Candidate
includes 54e800cd's W93 batching and correctness fixes, before the later ACK
ordering experiment. Repeats 2/3 use balanced candidate/baseline/baseline/
candidate order without concurrent builds. Pair 1 is retained but not used
for acceptance because a separate regression build may have overlapped.

Uncontaminated baseline close p99 is 237–249 ms, candidate 266–282 ms.
Median close improves in some samples, but the tail gate is NOT accepted.
A four-request bound experiment is retained separately; its first sample
is 287 ms p99, so merely shrinking the bound has not demonstrated a fix.
Later bound-4 samples may overlap a private build and are diagnostic only.
No default batch-bound change was made. Source/binary manifests accompany
each measurement. RSS samples are observed peaks, not proof of a global bound.

The follow-up tests ACK-after-log-durability before local state-machine apply;
these measurements do not describe that new ordering.

Durable-ACK-before-apply (`34fd56fa`) passes all correctness gates, but three
new isolated mixed-load samples have close p99 269–309 ms. This does not
resolve W53's end-to-end tail or establish a production performance win.
A same-current-code one-request control is the next attribution step.

The same-current-code one-request controls also show a large close tail,
so these short samples do not attribute the difference from the historical
baseline to batching. The test is being extended to 4096 tiny files with
alternating serial/batched ordering before any acceptance conclusion.
