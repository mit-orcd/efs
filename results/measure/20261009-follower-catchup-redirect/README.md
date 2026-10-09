# Redirect away from a follower that cannot apply its read index

The preceding transaction fault fixture exposed LINK EIO when the client's seed
was a follower whose transaction persistence was intentionally refused.
An authoritative remote ReadIndex succeeded, but local catch-up timed out.

The host now returns NOT_PRIMARY with that validated leader hint when this local
catch-up returns BUSY. It never serves the stale local view. Unknown, self or
out-of-range hints and transport/storage errors retain their original failures.
A native extracted-policy test checks these cases and the remote-authority gate.

AMD direct/TCP live gate restarts only the owned private seed so it rejoins as a
follower, then refuses its transaction persistence. Node1 retains15–18 pending
entries while LINK/rename complete through the leader route. Fault release
catches it up; cold restart preserves exact linked bytes and nlink2. Full POSIX
and physical reclamation are run in the same fixture. The earlier failed seed
case is retained in ../20261009-transaction-durability/seed-routing-fault.txt.

This closes the observed seed catch-up timeout case, not all W66 placement,
scaling or partition availability gates. AMD is one host/four daemons, and this
is injected persistence/process recovery, not hardware power-loss proof.
