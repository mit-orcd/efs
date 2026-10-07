# Roadmap rounds — Oct 7

Requested: 30 meaningful implementation rounds, each tested on the four-node
NUC. Mac testing is deferred during maintenance. Keep NUC direct I/O enabled.
D25 public logical truncate remains disabled pending its ownership/recovery
prerequisites. Each completed row records production changes, deterministic
regression checks and live NUC results; reviews and reruns do not count as
implementation rounds.

| Round | Production change | Tests | Commit |
|---|---|---|---|
| 1 | Truncate refuses failed flush; preserves inherited deadline and access errors | production regression PASS; NUC full POSIX 216/217, 64/64 peer, 12/12 persistence | d0d45b85 |
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
| 13 | One connection checkout budget includes wakeups, parent deadline and late-connect rejection | real checkout regression PASS; NUC concurrent and overlap gates PASS | accompanying client commit |
