# Every evidence script is cited by a page (Task EQ)

Task EQ, Nov 9 2026, Mac only, devel `33cd0a839` (wave 65). Purpose: the
structure plan's scripts triage bullet moved task-named scripts under
`scripts/evidence/<task>/` and relies on `check_doc_links.py` to prove the
citing pages were re-pointed. That proves page → script; nothing proved
script → page, so a script no page names (an orphan) could stay forever.

**Verdict: PASS, docs tooling and docs only, no K row, blocks alpha: no.**
`python3 scripts/check_doc_links.py` reports `0 dangling link(s), 0 orphan
script(s)`; it found 48 orphans on devel: 33 are now cited by the page whose
method, reproduction or plan runs them, 5 by the settings inventory, and 10
were deleted. None was deferred; the allow-list for pages Task EH owns was
not needed and not built.

## The check

Commit `c3c452e51`, `scripts/check_doc_links.py` (no new script; `make
docs-check` already runs it). After the dangling-link scan it lists the
tracked files under `scripts/evidence/` (`git ls-files`, so untracked
scratch is never flagged) and the tracked `docs/**/*.md` pages, and reports
each script whose full repo path (`scripts/evidence/<dir>/<file>`) appears
in no page. A link and a backticked path both contain that string, and
fenced blocks count (a reproduce block running the script is the best
citation). A bare basename does not count: the triage inventory
([scripts triage](scripts-triage-20261104.md)) names every moved file by
basename under a directory heading, so counting basenames would make the
check pass vacuously. Output keeps the dangling report's form, one line per
finding, then the summary:

```
<repo path of the script>: orphan script (no docs/ page names it)
check_doc_links: 0 dangling link(s), 1 orphan script(s)
```

Exit 1 on any dangling link or orphan. Negative control (Reproduce below):
with the one citation on the FIN page reverted to a basename the check
prints that line for the FIN audit script and `0 dangling link(s), 1
orphan script(s)`, rc 1; restored, rc 0.

This page names the orphans below as `<dir>/<file>` without the
`scripts/evidence/` prefix on purpose, so it does not cite them itself.

## The orphans and their fates

Last commit (`git log -1 --format='%h %ad %s' -- <path>`) was the triage
move `c4fab379a 2026-11-04 scripts: triage top level to 35 entries, move
task-named scripts under scripts/evidence/<task>/` for 47 of the 48, and
`a9433d914 2026-11-05 meta_apply split: evidence scripts and the graph
fixture path (Task DL)` for `dl/objcmp.py`. The table gives the commit
before the move (`git log --follow`), which is where the script came from.

| Script | Before the move | Fate |
| --- | --- | --- |
| `ac/ac_instrument.py` | `381fdc482` Oct 28 | cited, [AC](../2026-10/ac-iostats-rdma-scan-20261028.md) method (was basename) |
| `ac/ac_measure.py` | `5f9ddae71` Oct 28 | cited, AC method (helper of `ac_campaign.sh`) |
| `ac/ac_probe.h` | `381fdc482` Oct 28 | cited, AC method (header the instrumenter adds) |
| `af/af_instrument.py` | `f3e72d844` Oct 28 | cited, [AF](../2026-10/af-status-snapshot-20261028.md) build identity (was basename) |
| `af/af_measure.py` | `bc0854a75` Oct 28 | cited, AF (helper of the campaign scripts) |
| `af/af_probe.h` | `f3e72d844` Oct 28 | cited, AF build identity (was basename) |
| `an/an_base_control.py` | `05360d91d` Oct 30 | cited, [AN](../2026-10/an-put-status-batching-20261028.md) committed tools (was basename) |
| `an/an_private_tools.sh` | `05360d91d` Oct 30 | cited, AN (was basename) |
| `an/an_put_measure.py` | `05360d91d` Oct 30 | cited, AN committed tools (was basename) |
| `an/an_stat_client.c` | `05360d91d` Oct 30 | cited, AN committed tools (was basename) |
| `ao/ao_busy_probe.py` | `6997b2672` Oct 30 | cited, [AO](../2026-10/ao-readdir-pages-20261028.md) base diagnostic |
| `ao/ao_instrument.py` | `6fa231182` Oct 29 | cited, AO reproduction ("instrument a different archived copy") |
| `dl/objcmp.py` | (added in `a9433d914`) | cited, [DL](meta-apply-split-20261105.md) instruction-stream step |
| `fin/fin_accounting_audit.py` | `690169143` Oct 28 | cited, [FIN](../2026-10/fin-tcp-accounting-20261029.md) (was basename) |
| `intel/deepsteps_sitecustomize.py` | `06db13c9c` Oct 16 | cited, [Intel package 3](../2026-10/intel-package3-20261013.md) driver (helper of `intel_package3_tests.py`) |
| `intel/intel_package3_pressure.py` | `0e539bdbe` Oct 13 | cited, Intel package 3 baseline pressure gate |
| `k08/k08_overwrite_extend.py` | `23354597c` Oct 22 | **deleted**: only caller is `k08_stress_repro.sh`; the K8/W148 campaign ([gates-nuc](../2026-10/gates-nuc-20261022.md)) ran `w125_modify_repro.sh` |
| `k08/k08_stress_repro.sh` | `23354597c` Oct 22 | **deleted**: no page reports a run |
| `k08/k08_two_client.sh` | `1c47625bc` Oct 22 | cited, [settings inventory](../../how-it-works/settings.md) `EFS_RDMA_DEV` users |
| `orcd-mini/cs_lock.py` | `619f5297e` Oct 16 | cited, [waiter wake](../2026-10/orcd-mini-waiter-wake-20261016.md) reproduce (was basename) |
| `orcd-mini/orcd_mini_two_clients.sh` | `a1f2f79f4` Oct 20 | **deleted**: no page reports a run; W125 two-client runs used `orcd_mini_posix2.py` |
| `orcd-mini/task_k_mini_finalize.sh` | `e79dbb48a` Oct 27 | cited, [K44](../2026-10/k44-retired-replica-routing-20261026.md) final `SHA256SUMS` |
| `orcd-mini/task_k_mini_queries.py` | `1d3638e7a` Oct 27 | cited, K44 reproduction (was basename) |
| `orcd-mini/task_k_mini_ssh.sh` | `1d3638e7a` Oct 27 | cited, K44 reproduction (was basename) |
| `orcd-mini/task_k_mini_sweep.py` | `e56f63f82` Oct 27 | cited, K44 reproduction (was basename) |
| `orcd/orcd_dd_capture.py` | `f5b782726` Oct 28 | cited, [ORCD dd profile](../2026-10/orcd-test-dd-profile-20261028.md) reproduction (sampler of `orcd_dd_profile.py`) |
| `pump-logging/pump_logging_fullfs.sh` | `68303f40f` Oct 30 | cited, [AE product port](../2026-10/ae-product-port-20261030.md) (was basename) |
| `task-i/task_i_dispatch.py` | `931845748` Oct 26 | **deleted**: queues a run on nuc after Task H; host scheduling, not method; nuc withdrawn |
| `task-i/task_i_load.py` | `c6e61c921` Oct 27 | cited, [Task I](../2026-10/task-i-20261026.md) reproduction (D4 runner of `task_i_run.sh`) |
| `task-i/task_i_preserve.py` | `14010248c` Oct 28 | cited, Task I fixture-copy check (was `task_i_preserve.verify_fixture_copy`) |
| `task-i/task_i_wait_quiet.sh` | `f96c48a51` Oct 27 | **deleted**: quiet-host wait between sessions; host scheduling, not method |
| `task-k/task_k_after_attempt.sh` | `afc6d1466` Oct 26 | **deleted**: waits for an earlier attempt; host scheduling, not method |
| `task-k/task_k_surface.py` | `d27b2b13d` Oct 27 | cited, K44 `surface-00d426006` row |
| `task-n/task_n_append_summary.py` | `0acc5f302` Oct 27 | cited, [Task N](../2026-10/k15-k29-append-gates-20261027.md) reproduction |
| `w125/w125_big_node.sh` | `a1f2f79f4` Oct 20 | cited, [W125 closure](../2026-10/w125-closure-20261021.md) C0 (node side of `orcd_mini_big.sh`) |
| `w125/w125_churn.py` | `5f2bddd05` Oct 20 | cited, W125 closure C4 (writer of `w125_space_compact.sh`) |
| `w125/w125_mac_churn.sh` | `5f2bddd05` Oct 20 | cited, settings inventory packer rows |
| `w125/w125_mac_cluster.sh` | `17c9b5df6` Oct 23 | cited, settings inventory (cluster of the two Mac scripts kept) |
| `w125/w125_mac_concurrent.sh` | `1f2822e3c` Oct 20 | **deleted**: no page reports a run |
| `w125/w125_mac_mixed.sh` | `0e27beddb` Oct 20 | **deleted**: no page reports a run |
| `w125/w125_mac_pressure.sh` | `5f2bddd05` Oct 20 | cited, settings inventory packer rows |
| `w125/w125_mac_two_clients.sh` | `370e5e05c` Oct 20 | **deleted**: no page reports a run |
| `w125/w125_md5_compare.py` | `a1f2f79f4` Oct 20 | cited, W125 closure C0 (checker of `orcd_mini_big.sh`, also `w125_read_cost.sh`) |
| `w125/w125_packed_edges.py` | `f958852fd` Oct 20 | cited, [small-file packing closure plan](../../backlog/small-file-packing-closure.md) C3 (was basename) |
| `w125/w125_packed_namespace.py` | `4353ce98e` Oct 20 | cited, packing closure plan C2 (was basename) |
| `w125/w125_pause_node.sh` | `d7ddcc045` Oct 20 | **deleted**: no page reports a run |
| `w125/w125_read_pressure.sh` | `d7ddcc045` Oct 20 | cited, settings inventory `EFS_DCACHE_*` users |
| `w166/w166_trace_summary.py` | `c71677190` Oct 26 | cited, [W166](../2026-10/w166-close-eio-flush-deadline-20261026.md) forensic ticket counts |

Rules applied. A page already naming the script by basename got the full
path in place. A helper that a cited script calls or includes is cited on
the page that cites its caller, with one clause. A script whose task page
describes the run it implements is cited there. The settings inventory
names five scripts as the users that keep packer, buffer and RDMA knobs
until A8; it is their rightful citer, so their basenames became full
paths there (cell text only; `gen_settings.py` and `env_inventory.py` do
not read that column). A script with no reported run and no caller was
deleted: three host-scheduling wrappers for withdrawn hosts and seven
uncited load scripts. Before deleting, `git grep` over the tree (outside
`graphify-out`) found no use of any of the ten except the triage
inventory and, for `k08_overwrite_extend.py`, its deleted caller; they stay
in history at `c4fab379a`.

None of the orphans' rightful citers is `docs/status/README.md`, the
structure plan or a page Task EH's unmerged wave 66 branch rewrites, so no
orphan was deferred and no allow-list comment was added.

## Reproduce

On a checkout of this branch:

```sh
python3 scripts/check_doc_links.py      # 0 dangling, 0 orphan, rc 0
make docs-check
# negative control: un-cite one script, expect rc 1 naming it, then restore
E=scripts/evidence   # split so this page does not cite the script itself
sed -i '' "s|\`$E/fin/fin_accounting_audit.py\`|\`fin_accounting_audit.py\`|" \
  docs/status/2026-10/fin-tcp-accounting-20261029.md
python3 scripts/check_doc_links.py; git checkout -- docs/status/2026-10/fin-tcp-accounting-20261029.md
```

(`sed -i ''` is the macOS form; on Linux use `sed -i`.)

## Limits

- Citation is textual: a page naming the path proves a reader can find the
  script, not that the page's method still runs it correctly.
- The [scripts triage](scripts-triage-20261104.md) inventory is a dated
  record; its per-directory counts predate the ten deletions and were left
  as they were.
- Only `scripts/evidence/` is checked. The triage bullet moved task-named
  scripts there; the 35 remaining top-level `scripts/` files are tools and
  checkers that `Makefile`, tests or agents call, outside this check.
