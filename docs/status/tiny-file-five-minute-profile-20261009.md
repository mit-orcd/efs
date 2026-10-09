# Tiny-file sustained throughput investigation — 2026-10-09

Five-minute NUC profile of deployed build `800122c5e86d`, all four daemons,
main FUSE client and eight-worker generator: 14,954 samples, zero lost.
No production code or deployment changed.

The generator reports a cumulative average. During the captured 300 seconds,
actual 100-second interval throughput recovered 39.64 → 47.48 → 61.15 files/s,
while the displayed average declined 207 → 72 files/s. The initial approximately
215 files/s burst nevertheless falls to a much lower sustained rate.

The strongest evidence points to synchronous persistence latency on the shared
NVMe backing n3/n4: 89–93% busy, write await 3.26–4.77 ms, and a separate
post-capture n3 trace found fragment fsync mean 19.59 ms and WAL fdatasync
mean 7.79 ms. Python and FUSE CPU are low. This is not proof of a hardware
ceiling, and the reason for increasing device latency remains unresolved.
Measure existing WAL batching effectiveness and fragment persistence costs,
then compare separate-device placement while retaining durability guarantees.

W92 has a separate scale issue: catalog accounting reached the unchanged
1024 MiB cap after about 191 seconds of recording, leaving stale snapshots.
This occurred after the initial slowdown and does not explain its onset.
Do not declare scale acceptance complete or silently raise the cap based on
this profile alone.

Raw data, methodology, limitations and detailed measurements:
[five-minute evidence](../../results/measure/20261009-tiny-five-minutes/README.md).
The dedicated generator exited after SIGINT and queued work drained; its
test data remains under `/data1/efs/mnt/tiny-profile-five-minutes`.
