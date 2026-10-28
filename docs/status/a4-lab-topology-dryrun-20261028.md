# Task V — four distinct host TCP driver rehearsal

Branch `agent/devel-task-v`, base `b2d57e421`: IN PROGRESS. Matrix validation
PASS; Linux driver tests and live gates pending. The user selected a private
additional cluster on fcstor012–015 instead of reserving the four lab hosts,
and explicitly requested its destruction and cleanup afterward. A4 remains
an open release decision; this is driver preparation, not A4 acceptance.

## Scope and source changes

`fcstor-4x1-tcp` describes one member per host on wired addresses
10.1.223.66–69, port 19340, two mounts (fcstor012/013), TCP, O_DIRECT,
packing and default commands policy enabled, expiry off, 4 GiB/member.
Stores/mounts are under `/tmp/efs-taskV-20261028/cluster`; source and build
scratch are siblings. Fixture sizes match `loopback-5`, including 2,000
small files and two 32 MiB large files per member. No user cluster is used.

Driver fixes under review: two-client remote execution receives each host's
actual script path; teardown requires clean client stops and daemon stops
before removing stores and propagates cleanup failure into the run verdict.
The Task V SSH wrapper restricts destinations to the four authorized hosts
and does not use BatchMode. Candidate placeholders resolve only to the exact
explicit `--candidate` build.

## Prerequisites and initial inventory

All four machines are reachable through the ORCD login, have 24 CPUs,
RHEL 9.8 kernels, `/dev/fuse`, and about 404 GiB free on local `/tmp`.
No efsd process or listener in the proposed range was found. Wired interface
`ens10f0np0` has 10.1.223.66–69; InfiniBand 172.16.223.66–69 is excluded
from this TCP rehearsal. System libfuse 3.10.2 is too old; an existing private
libfuse 3.16.2 is available at
`/home/erbmi1/orcd/scratch/efs/orcd-test/deps/libfuse` (read-only dependency).
No packages are installed. Exact build identities, gate walls, raw paths,
hashes, failures and cleanup receipts will be recorded after execution.

## Gates

`identity`, `topology`, `b-checks`, `posix-core`, `posix-2client`,
`c5-protocol-live`, `e1-manifest` (orderly restart of this owned cluster),
`d3-efs-load`, and `e4-scale`. No timeout is relaxed and every failure stays
reported. RDMA, crash/power-loss and full A4 release acceptance are NOTRUN.
