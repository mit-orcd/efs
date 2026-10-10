# Private four-voter host integration gate

AMD single host, direct-I/O/TCP, private port range 31980–31983 and unique stores
`w66-four-voter-direct-20261010-v2`. Installed cluster unchanged.

Both existing metadata groups explicitly reconfigured through `raft-change`
from their legacy three-voter placement to mask 0xf. The harness waits for each
of all four hosts to report that group hosted, voters=0xf and joint=0; a CLI
return alone does not satisfy convergence. Results include initial and cold
status for every host, complete configuration replies and all read samples.

800/800 GETATTRs pass. Idle cell p50 0.94–0.95 ms, p99 1.25–1.46 ms;
write-loaded p50 1.47–1.51 ms, p99 2.40–2.99 ms, including CLI startup.
Both group leader-loss/rejoin cases and cold restart pass (36 exact inode
identity probes). Cold statuses retain four-voter membership on both groups.
A first harness attempt incorrectly waited for both groups before configuring
the second; it timed out and gracefully stopped its owned daemons. The corrected
per-group predicate produced this passing run; no runtime fix is attributed to
that harness error.

This proves explicit four-voter host/engine integration and single-leader loss,
not authoritative two-loss recovery, distributed failure domains, independent
metadata capacity or a speedup. Both groups initially elected the same node;
more replicas alone do not distribute leadership. No production activation.
