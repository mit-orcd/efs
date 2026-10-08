# D17, D26/W44 and W54/W38 NUC acceptance

Private four-node TCP fixtures on SATA and NVMe storage. Logs and manifests
retain failures separately from repaired runs. Test-only MPI/IOR and ecrawl
builds reside under /data1/efs on the NUC; shared service stores were untouched.

Actual ecrawl: `gc-direct-0txt8cum` and `gc-buffered-90u9w230`.
Actual nine-client IOR: `gc-direct-fa4aux1l`; four-/36-rank direct
`gc-direct-uy9gzgxy`; repaired buffered `gc-buffered-pebmqq73`.
Ten-GiB bulk: `gc-direct-kbglf4b1`; current harness one-GiB confirmation
`gc-direct-opjr9ilf`. Full remote fixture paths begin `/data1/efs/`.
`remote-log-hashes.json` retains hashes of large remote traces.
`ior-direct-drain` records exact-binary-guarded zero physical usage.
`retained-settled` contains all three metadata samples and server pass logs.

Initial direct IOR cleanup exceeded its 600-second timeout; original bulk
fixture hit the client retirement race. Those failure logs are retained.
Older reused scratch builds are excluded from acceptance; successful ecrawl
and one-GiB repeats use unique `/data1/efs/gate-clean-RJ2Plx`.
The clean Linux unit suite uses a fresh archive of committed source and passes.

Tool provenance: ecrawl source commit
`7308ed67c352ec3157e1ee3a8672a35cb45dbf25`; IOR 4.0.0; MPICH 4.2.3.
Binary hashes and exact invocation geometry are in fixture manifests.
The frozen idle-hour script preserves its launch identity; a separate helper
overlay repairs retirement without changing its C binaries or loaded harness.
