# AMD v0.2.0-pre-alpha acceptance

Merged source commit `2ba0d7cc` (merge `952b57d5`), full clean Linux build and
`make test`. Deployed source hashes match349 tracked build/test files exactly.
Build metadata supplied explicitly because a copied worktree .git file cannot
resolve on AMD. All processes use the same candidate binary hashes.

Each mode runs full POSIX, two-client POSIX2, then26 durability prepare and26
cold-remount verify cases. The runner verifies all four daemon I/O flags, both
client transport settings and uverbs presence for all six RDMA processes (and
absence for TCP). Strict RDMA uses active RXE rxe0/GID1 on AMD: software RoCE,
not hardware offload or cross-host/native-IB acceptance. mmap remains the one
known unsupported POSIX skip. Matrix and TSVs contain exact verdicts.

The durability driver formerly forced TCP on remount; its fix is committed in
the devops repository as `e0fcc1f`. The runner invokes remote scripts with
explicit settings so their SSH re-exec cannot lose the matrix configuration.
No force-discard, reformat or test-only filesystem behavior was used.
