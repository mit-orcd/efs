# S4 / W122: effective-settings listing (Oct 30)

**Verdict: GATE PENDING** (updated below when `make test-all` finishes).

- **Purpose:** checklist [S4](alpha-1.0-2plus1-checklist.md), work item W122,
  decision D46 item 3: a minimal, bounded, read-only listing of effective
  settings (values, defaults, provenance, active policies, transport, build).
  No new behavior knob, no write form.
- **Tested source:** branch `claude/devel-bf-s4-683b1c` cut from devel
  `7514b2145`; the head commit is recorded in the Gate section.
- **Host:** xorinox (16 CPUs, Linux, TCP only; FUSE available), private tree
  `~/efs-bf` under `$HOME`; loopback clusters on free ports only.

## What was built

- `efs-mgmt settings NODE:PORT`: wire message `EFS_MSG_SETTINGS` (247/248),
  answered by `src/server/handler.c` from the table in `src/common/settings.c`.
  One reply of at most 8 KiB, tab-separated lines `name value default
  provenance kind` sorted by kind then name, after a header that counts the
  observability (34) and fault/test (62) settings it leaves out. The command
  adds the cluster `.efs` commands policy and the cluster expiry policy
  (read through the same node, like `commands-policy get` and
  `expiry-policy cluster get`; their rows are `policy`). The daemon adds
  `durability-window`, `transport`, `direct-io` and `build`.
  Exit status 1 when the node refuses, is unreachable, the reply is
  truncated or a policy cannot be read.
- Covered: the 9 deployment inputs and the numeric tunables of the A6
  inventory (the page lists 34 numeric tunables, one more than the 33 in the
  D46 text: `EFS_PUT_AWAY_MARK_MS`, a client setting) and `--writers`, the
  one tunable on the `efsd` command line. Daemon rows are the daemon-side
  settings, client rows the client-side ones; settings both read (RDMA,
  `EFS_DIR_SPREAD_MIN`) are in both.
- Recording: `efs_settings_init()` at process start fills every row from the
  environment (or the default); each reader then calls `efs_setting_u64()` /
  `efs_setting_str()` once with the value it resolved, so clamped or refused
  input shows the effective value and `built-in` provenance. A setting read
  in several places has one row. Readers noted: packer (`env_u64`), client
  budgets (`budget_env`, `report_env`, `EFS_DCACHE_BYTES`,
  `EFS_CLIENT_META_MB`, connection pool, view poll, resolve, chunk-map TTL),
  RDMA (buffers, port, GID index, device, transport), query cache limits,
  `EFS_DIR_SPREAD_MIN`, and in the sibling-owned files one line each in
  `raft_host.c` (GC backlog, applied interval, rebuild pass, REPORT receipt
  join, query refresh, query export) and `store.c` (PUT sync window).
- Client listing: `efs-mgmt settings --mount MOUNTPOINT`, a `SETTINGS`
  command on the private control socket `efs-fuse --stop` already uses
  (`src/client/stop_control.h`). Chosen over a file under `.efs` because `.efs`
  appears only while the commands policy enables it and would need a new
  virtual-file path in the FUSE layer; the socket is owner-only (0600).

## Limitations

- Rows read lazily (RDMA receive buffers, packing tunables before the first
  pass) show the environment text or the default until the first read.
- The policy rows come from two RPCs to the same node, not from the daemon's
  recorded state: they live in the replicated state, not in the process.
- Observability and fault/test settings are counted, not listed.
- `EFS_NUMA_NODE` appears in the client listing only after the client has
  pinned (otherwise `auto`).
- No JSON form and no portal section (W122's beta remainder).

## Tests (in the suite)

- `tests/test_settings_inventory.py` (`make test`): the table's `EFS_*` rows
  equal the A6 inventory's deployment-input and numeric-tunable names, the
  header counts equal the inventory's observability and fault counts, and a
  table or inventory missing one row fails (negative controls).
- `tests/test_settings_listing.py` (`make test-long`, budget row in
  `tests/budgets.tsv`): three real daemons; node 1 with two tunables in the
  environment and `--writers` on the command line lists `environment` /
  `command line` and every other row `built-in`, `automatic` or `policy`,
  node 2 the defaults; the policy rows follow `commands-policy set` and
  `expiry-policy cluster set`; unreachable node and bad arguments exit
  non-zero; a mounted client lists its direct-I/O, transport and budget rows,
  with a budget set in its environment.

## Reproduce

```bash
git archive HEAD | ssh HOST 'mkdir -p ~/efs-bf/src ~/efs-bf/tmp && tar -x -C ~/efs-bf/src'
ssh HOST 'cd ~/efs-bf/src && export TMPDIR=$HOME/efs-bf/tmp && make -j16 && \
  python3 tests/test_settings_inventory.py && python3 tests/test_settings_listing.py'
ssh HOST 'cd ~/efs-bf/src && export TMPDIR=$HOME/efs-bf/tmp && SCREENDIR=$HOME/.screen \
  screen -dmS bf-gate env EFS_TEST_JOBS=8 scripts/make_test_summary.sh'   # 13-20 minutes
```

Cleanup: `~/bin/edelete --force --delete ~/efs-bf`. Raw runner output stays in
`xorinox:~/efs-artifacts/bf-s4/` (outside Git).

## Gate

GATE PENDING.
