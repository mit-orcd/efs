# W66 Mac buffered/TCP evidence

Private four-daemon fixtures on Mac cluster VM efs1. Installed services untouched.
Candidate 2b9da49f; baseline replaces only raft.c with pre-fix 532660e3 version.
Run order latency-a1, latency-b1-retry, latency-b2, latency-a2. Each results.json
contains all GETATTR samples, mutation attempts and before/after Raft status.
The initial b1 start hit TIME_WAIT; no measured samples were used from it.

Full make test and stale-leader freshness passed. POSIX failed: 145 pass, 7 fail,
60 not run out of 212. Peer/final recovery not reached. Logs and fixture manifest
preserve that failure. No general latency improvement or live acceptance claimed.
