# devel-find-stats — all committed branch integration

Oct 9 EDT / Oct 10 UTC. Merge boundary `f4f95ed7` includes every local branch
tip listed in the [boundary ledger](../../results/measure/20261009-find-stats-merge/branch-boundary.txt).
This imports committed work only; other worktrees and their uncommitted edits
are left intact. No push, tag, main merge or service redeployment.

Three merges bring in the missing work:

- `9e87533a`: `devel` at `fee363f5`, including integrated writeback/replay,
  publication, correctness and documentation work.
- `5e0fe113`: `devel-w66` at `70daa5a4`, adding retained PUT admission manifests
  and durable mapping receipt bindings.
- `f4f95ed7`: `devel-persistence-pressure` at `437b7890`, including opt-in inbox
  credit retirement and confirmed-absence stale-map recovery.

The W92 cache optimizations are retained. Generated architecture HTML is
regenerated from merged source docs. Read conflicts preserve both NOT_FOUND and
DECODE absence cases, failed-authority refusal and retained staging. Pump conflicts
combine the credit-retirement helper with newer checkpoint-failure log suppression;
shutdown still requires the shared KV-before-marker durability tail. Focused
regressions exercise these combined behaviors.

All five architecture gates and the focused read/retirement regressions pass.
Full native Linux `make all` and `make test` pass (exit code 0); results are recorded in the
[validation ledger](../../results/measure/20261009-find-stats-merge/README.md).
This merge does not close W55 NFS acceptance, W66 topology/routing or W92 full
incremental derived-stat/scale obligations. Deployed Xorinox remains `b4212453`;
new merged publication/recovery behavior has not been deployed or live-tested
by this integration round. [Status index](README.md).
