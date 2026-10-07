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
| 3 | Writeback workers inherit budgets, bound overlap/stripe waits, reject expired admission and complete cleanup | worker/queue/memo regressions PASS; NUC concurrent 8/8 and peer overlap 3/3 PASS | accompanying client commit |
