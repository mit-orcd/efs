# W93 longer production mixed-load gate

AMD direct I/O/TCP, one host/four daemons, private 4 GiB/node stores. Six isolated fixtures ran 4,096 tiny 512-byte CREATE/close operations with four bounded 16 MiB large-file rings. Order: ACK, serial, serial, ACK, ACK, serial. Serial uses the same code with only the host receive coalescing limit changed to one. Builds and other tests were excluded from measured intervals.

| Mode | Wall seconds | Close p99 ms | Mean close p99 ms |
|---|---|---|---|
| ack | 34.648, 33.684, 35.252 | 330.271, 288.571, 308.529 | 309.124 |
| serial | 33.470, 34.263, 34.560 | 309.686, 334.450, 290.226 | 311.454 |

No consistent mixed-load tail regression appears in these three balanced pairs; mean close-p99 differs by less than one percent and samples overlap substantially. This is not proof of universal latency improvement. Wall time and large-writer bytes vary; this closed-loop workload is not a fixed large-writer arrival rate. Core/disk batching gains are documented separately.

All six fixtures cold-restarted, checked every tiny/large byte, deleted records and waited for physical GC queues/bytes to drain. Installed AMD service was restored after each fixture. Manifests retain source/binary identities.

An earlier 1 GiB/node run exhausted quota while bounded GC lagged writes. It is excluded from timing comparisons. Observation of that preserved fixture showed pending work drain to zero without GC errors; increasing this test quota does not fix production admission/GC-pressure behavior.
