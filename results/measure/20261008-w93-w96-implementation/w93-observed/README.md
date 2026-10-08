# Separate W93 pump/barrier observation

AMD direct I/O/TCP, 512 tiny CREATE/close operations under four bounded large
writers. Existing EFS_RAFT_OBS diagnostics were enabled only for private test
daemons. Cold bytes and physical reclamation passed; installed service was
restored. This diagnostic run is separate from the balanced timing series.

Across logged intervals, apply wait timeouts and inbox drops were zero. Largest
pump hold/drain were 15.902 ms; largest lock wait 23.700 ms; largest persistence
maximum 1.824 ms. Outbound queue high-water was three. Close-p99 was 295.424 ms.
These figures do not identify a single pump/barrier stall explaining that tail;
multiple stages/data-plane waits remain possible. Counter maxima cover the
whole fixture including restarts, not a per-close trace or proof of a global
latency bound. Two recorded transport failures are retained in the raw logs;
the fixture deliberately cold-restarts its daemons, so aggregate counts cannot
be attributed to the measured close interval without further correlation.

No batching limit was changed to hide the tail. W53 end-to-end close latency
remains a separate investigation; W93 same-code long samples overlap and its
controlled durable-group batching gains remain repeatable.
