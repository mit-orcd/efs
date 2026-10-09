# W97 initial backpressure candidate, 2026-10-09

Source 9728d07b; uniform build 9728d07b-flow. Flow enabled (EFS_RAFT_FLOW=1),
TCP/direct I/O, commands disabled. Preserved stores, fresh seeded tiny-file
output tree, 8 workers, five minutes and cooperative SIGUSR1 stop/drain.
No compile, functional suite or profile attach overlaps the timed workload.
This is sequential exploratory evidence, not repeated performance acceptance.

inbox_drop now counts soft-watermark refusals as well as hard overflow.
wire_sent counts successful physical transport sends; flow_busy and
flow_admitted count parsed admission replies. These replies grant no Raft
or durability authority. Candidate retains actual core ACK/read semantics.

Raft/store/wire, actual host sender/admission async fixture and group-isolated
outbox tests passed before deployment. Functional and sustained results are
recorded separately. A follow-up optimization under development after observing
frequent refused enqueues is not present in these measured binaries.
