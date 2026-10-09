# Simultaneous NUC efsd CPU profile

Installed build 093295be288b, direct I/O/TCP, .efs enabled. Active eight-worker
tiny-file workload, output tiny-ordinal-093295aa, target 20,000. No workload,
code or deployment changes for this capture. 30-second perf at 199 Hz across
the four production PIDs, 6,814 samples, zero lost. /proc counters bracket
31.951 seconds including perf startup/teardown. Reports use relative
percentages per filtered PID; inclusive rows overlap and must not be summed.

| Node | PID | CPU, one core=100% | Main inclusive sampled paths |
| --- | --- | ---: | --- |
| 1 | 1946342 | 11.4% | TCP send/message 25.63%, request handler 23.50% |
| 2 | 1948386 | 58.0% | query worker 73.86%, catalog builder 72.52% |
| 3 | 1950375 | 10.5% | query worker 23.50%, Raft pump 17.55%, fragment writes 12.76% |
| 4 | 1952769 | 4.8% | fragment writes 28.33%, Raft pump 22.86%, fsync path 12.74% |

Node 2: builder self 30.12%, add_node inclusive 30.31%, page-fault entry
24.10%, anonymous-page handling 14.06%, clock_gettime 5.89%, unmap 5.46%.
Page-fault/unmap costs support targeting repeated global inode-vector/index
allocation and reconstruction. Per-row stopped() invokes host_query_cancel,
which reads the monotonic clock; bounded batching of deadline checks is a
smaller candidate, preserving prompt shutdown and bounded cancellation.
Architectural partition-local immutable inode/stat blocks could avoid
reconstructing unchanged rows; lifetime, lane dependencies, memory caps,
bindings and rollups require differential and mutation/restart gates.

Node 1 shows dispersed socket/kernel work rather than one dominant user
function. Consider message/batch counts and CREATE critical-path latency
before changing batching or synchronization. Nodes 3/4 show replication and
durable writes, but low CPU does not establish disk latency or saturation.
Perf measures on-CPU work; blocked sync waits require separate off-CPU/syscall
latency and device evidence. Total measured daemon CPU is 84.7% of one core,
not the entire host. No hardware-bound throughput claim or code fix here.

Latest publication excerpts approach 1.052 GB against the 1 GiB catalog cap;
the snapshot has around 1.12 million files. Keep growth bounded. RSS around
2 GiB on catalog-hosting nodes includes retained/old/candidate/source memory;
catalog accounting is not process RSS.

Raw perf data, per-PID self/inclusive reports, build/PID identities, /proc
counters and publication excerpts are retained in this directory.
