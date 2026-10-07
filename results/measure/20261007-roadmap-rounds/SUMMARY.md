# Roadmap rounds — Oct 7

Requested: 30 meaningful implementation rounds, each tested on the four-node
NUC. Mac testing is deferred during maintenance. Keep NUC direct I/O enabled.
D25 public logical truncate remains disabled pending its ownership/recovery
prerequisites. Each completed row records production changes, deterministic
regression checks and live NUC results; reviews and reruns do not count as
implementation rounds.

| Round | Production change | Tests | Commit |
|---|---|---|---|
| 1 | Truncate refuses failed flush; preserves inherited deadline and access errors | production regression PASS; NUC full POSIX 216/217, 64/64 peer, 26/26 persistence | d0d45b85 |
| 2 | Refuse truncated-prefix rewrite on read failure/short prefix; require REPORT before SETATTR | reproduced old failure; regression PASS; NUC full POSIX, peer and cold persistence PASS | d83f7d44 |
| 3 | Writeback workers inherit budgets, bound overlap/stripe waits, reject expired admission and complete cleanup | worker/queue/memo regressions PASS; NUC concurrent 8/8 and peer overlap 3/3 PASS | c265b009 |
| 4 | One FUSE write budget includes pressure drains and append locking in both entry points | write/cache/memo regressions PASS; NUC concurrent 8/8 and peer append 3/3 PASS | a846ce39 |
| 5 | PUT-window read wait respects monotonic inherited budget | window regression PASS on Mac and NUC; live single/peer straddle 1/1 each PASS | cf54a4e9 |
| 6 | Bound REPORT retry/slot waits, reject expired work, restore TLS and measure actual sleeps | retry/pressure regressions PASS; NUC fsync 4/4 and peer overlap 3/3 PASS | fbaf6046 |
| 7 | Surface read metadata/PUT-window failures instead of false EOF or holes; define failure byte count | read admission regression PASS; NUC read-filter single/peer gates PASS | 5868ec73 |
| 8 | Stop expired GETCHUNKS work; fix session-wide append sequence race found during full gate | initial full gate lost one append record; old counter fails concurrency regression; fixed NUC 216/217 + 64/64, three 8/8 concurrent repeats PASS | e5bb19c4 |
| 9 | Bound GETCHUNKS retries and free failed receive payloads before releasing ownership | RPC fault regression PASS; NUC read and peer-overlap gates PASS | 17be9144 |
| 10 | Prevent chunk-group end from wrapping at the upper uint32 boundary | serial/fan boundary regression PASS on Mac and NUC; live sparse single/peer gates PASS | 8996f08f |
| 11 | Stop STALE replay fanout after budget expiry without replacing earlier errors | replay/classifier regressions PASS; NUC concurrent and peer-overlap gates PASS | 098e1a3f |
| 12 | Bound PUT dispatch, wake shard workers immediately, refuse shutdown admissions and recover failed pool startup | inline/full/shutdown/startup/one-slot regressions PASS; NUC concurrent and overlap gates PASS | 6d105c58 |
| 13 | One connection checkout budget includes wakeups, parent deadline and late-connect rejection | real checkout regression PASS; NUC concurrent and overlap gates PASS | 54a36da8 |
| 14 | Bind RPC scopes to absolute TCP receive deadlines, including slow-drip frames | network regression/wire/fd checks PASS; NUC full 216/217 and 64/64 peer PASS | e541c823 |
| 15 | Absolute TCP send deadline bounds full and partial frames; Darwin nonblocking flags restored on return | TCP fault regression PASS on Mac and NUC; NUC concurrent 8/8 and overlap 3/3 PASS | 0be303d9 |
| 16 | Bound TCP connection establishment across interrupted waits and reject late success; preserve connect/flag errors | connect fault regression PASS on Mac and NUC; live concurrent 8/8 and overlap 3/3 PASS; hostname resolution remains outside this bound | e8dc2302 |
| 17 | RDMA reply waits share absolute RPC budget; late successful frames return pool ownership before rejection | wrapper fault regression PASS on Mac/NUC; live NUC read/overlap gates PASS over TCP; live RDMA hardware gate remains owed | f281e4ce |
| 18 | Bound shard/dual metadata retries, reject late replies, free failed receive allocations and measure actual retry sleeps | RPC fault regression PASS on Mac/NUC; full NUC 216/217 single and 64/64 peer PASS | 742c6128 |
| 19 | Global/per-inode writeback drains use remaining budgets and retain pending work/errors on timeout or wait failure | real drain/worker regressions PASS; NUC fsync 4/4 and peer overlap 3/3 PASS | 09f1a4de |
| 20 | Append flush refuses expired free-stripe admission and post-drain expiry; reservation diagnostics added | regression PASS; initial concurrent gate lost first append record (still unresolved); isolated 100/100 and subsequent concurrent 10×8/8 PASS; peer append PASS. Failure trace retained, not exonerated by repeats | 44e2a855 |
| 21 | Serialize complete server append replay entries; obtain allocation offsets from the leader and refuse generic offset-less forwarding after a role change | old cache reproduces wrong offsets; authority/cache regression PASS on Mac/NUC; full NUC 216/217, 64/64 and cold persistence 26/26 PASS. Further churn/replay gates remain needed; this does not activate D25 | ec1500ec |
| 22 | Demand read scratch retries stop at inherited expiry and distinguish timeout from allocator exhaustion | pressure regression PASS on Mac/NUC; live NUC read and overlap gates PASS; full source-rebuilt NUC unit suite PASS after round 21 | 22bc6a11 |
| 23 | Zero-copy read refuses failed PUT-window waits before pinning cached images | production refs regression PASS on Mac/NUC; live NUC read and overlap gates PASS | 1cb09de0 |
| 24 | Validate copy/zero-copy read extents before uint32 chunk casts; reject invalid pin arrays | production overflow/pointer regressions PASS; NUC sparse single/peer gates PASS | 924195e6 |
| 25 | Inode writer-view authority acquisition shares whole retry budget and refuses late replies | RPC fault regression PASS on Mac/NUC; NUC concurrent 8/8 and overlap 3/3 PASS | d02c087c |
| 26 | Validate readdir counts and progressing terminated cookies; reject late pages and free failed buffers before output adoption | malformed reply regression PASS; NUC readdir/listdir gates PASS | bed7a83e |
| 27 | Bound REPORT wire arithmetic and validate pointers/counts/delta geometry before allocation; stop expired array walks | frame boundary regression PASS; NUC fsync 4/4 and overlap 3/3 PASS | d2724fed |
| 28 | Shard completion surfaces delayed close errors without retrying a possibly released descriptor | actual buffered/direct syscall-fault regression PASS on NUC; full 216/217, 64/64 peer and 26/26 cold persistence PASS | 44418279 |
| 29 | GET-pool startup cleans partial mutex/condition/thread failures; publish readiness atomically and destroy quiescent resources before restart | real startup/stop/restart fault regression PASS on Mac/NUC; live NUC read and overlap gates PASS | 68dff535 |
| 30 | GET workers inherit batch deadlines; queue/cache waits and fragment retries stop on expiry while accepted jobs retain stack ownership until completion | real one-slot queue, expiry, shutdown, prefetch cleanup, pending-cache and retry regressions PASS on Mac/NUC; live NUC read 18/19 (one expected mmap skip), peer overlap 3/3 PASS | fb9ed871 |

Final review also found metadata lookahead wrapping at the last representable chunk range, incorrectly skipping its pull. Saturated lookahead and widened comparisons pass a production boundary regression and the NUC sparse single/peer gates; this follow-up is separate from the 30 counted rounds.

## Final acceptance — NUC direct I/O

Production rollout: `128f6b7d` (30 rounds through `fb9ed871`, plus the
read-lookahead boundary follow-up). All four running daemons have identical
SHA-256 `d35b0534af7325d2e9b5c4d328a964a07560c3a2af08f276bec60a5e324a5619`
and `--direct-io`; n1/n2 use SATA and n3/n4 use the NVMe-backed location.

| Gate | Result |
|---|---|
| W36 first on final rollout | 20/20 rename-versus-unlink repeats PASS |
| Forced source rebuild and complete Linux unit suite | PASS |
| Full single-client POSIX | 216 pass, zero fail, one unsupported mmap skip, zero NOTRUN |
| Full two-client POSIX | 64/64 PASS |
| Persistence prepare and clean-unmount/cold-remount verify | 26/26 each PASS |
| Concurrent suite repeated after full acceptance | 10 × 8/8 PASS, including both append cases |
| Architecture documentation gate | 5/5 PASS |

Raw logs and per-test TSVs are under [final](final/). The unit run rebuilt
on-disk binaries with a dirty marker for a test-fixture-only change; production
sources were unchanged. Running server versions/hashes are recorded separately.
The fixture now extracts the actual pending-cache wait code rather than copying
it. Persistence counts in the initial rollout and round summaries were corrected
from 12 to 26 using their retained raw logs.

The append recurrence found during round 20 remains evidence, even after these
clean repeats. Round 21 proves and fixes replay-cache entry races and obtains
allocation replies from the leader. Durable reservation replay across leader
changes and broader churn gates remain open; these results do not establish a
sole cause for the captured loss.

Both clients drained cleanly during deployment. Nuc n1 twice exceeded the
stop script's ten-second SIGTERM wait and required SIGKILL. This shutdown blocker
needs thread-stack/strace capture during a controlled stop and a separate fix;
no graceful-daemon-shutdown acceptance is claimed.

D25 is still staged: typed FUSE admission and both flush paths, bounded
materialization/history retirement, and activation gates remain. D27 strict
whole-call timing and live fault/recovery/RSS gates remain open. DNS and active
job ownership still prevent a hard wall-clock completion guarantee; RDMA needs
its hardware gate. Mac buffered acceptance is deferred during maintenance.
