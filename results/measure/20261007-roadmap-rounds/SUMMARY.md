# Roadmap rounds — Oct 7

Requested: 30 meaningful implementation rounds, each tested on the four-node
NUC. Mac testing is deferred during maintenance. Keep NUC direct I/O enabled.
D25 public logical truncate remains disabled pending its ownership/recovery
prerequisites. Each completed row records production changes, deterministic
regression checks and live NUC results; reviews and reruns do not count as
implementation rounds.

| Round | Production change | Tests | Commit |
|---|---|---|---|
| 1 | Truncate refuses failed flush; preserves inherited deadline and access errors | production regression PASS; NUC full POSIX 216/217, 64/64 peer, 12/12 persistence | accompanying client commit |
