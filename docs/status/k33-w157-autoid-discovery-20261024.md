# K33/W157 increment 2 — automatic node ids, positional CLI, discovery (Oct 24, 2026)

**Verdict: code gate PASS with host limits (`make test-all` on xorinox at
`53080d744`: 436 commands, 0 FAIL, 13 NOTRUN host limits); model PASS.
Live multi-host, cross-subnet, RDMA and orcd-mini gates NOTRUN (the parent
session's). Branch merged into devel (`60f91b0d3`) (base `merge-k32-k33`
`2d03d9837`), not merged, not pushed.**

Purpose: increments 1 and 2 of "Remaining increments" in the
[increment 1 evidence](k33-w157-formation-addition-20261023.md) for
[K33](alpha-1.0-2plus1-checklist.md) / [W157](work-queue.md), an alpha 1.0
blocker: the minimal startup of the
[contract](alpha-1.0-node-addition-20261023.md#normal-operator-interface)
with no operator node ids, and discovery. Protocol:
[k33-membership-protocol-20261023.md](k33-membership-protocol-20261023.md)
(M1, M2, M4′, M5, M9 updated for this increment).

## Operator interface (built)

```sh
efsd 10.0.0.11:17432 /data/efs                  # new cluster in fresh storage, or restart
efsd --join /data/efs                           # join a cluster found on the local subnet
efsd --join 10.0.0.11:17432 /data/efs           # join through any member, across subnets
efsd --join 10.0.0.11:17432 --listen 10.0.1.12:17432 /data/efs   # name this node's endpoint
```

- No node id, member count or cluster file. `--node-id`, `--addr`,
  `--port` and `--storage` are removed; efsd refuses them by name. STORAGE
  is one positional argument (roots separated with commas).
- A fresh store started with `ADDR:PORT` is a **seed**: node 1 at that
  endpoint, a 64-bit cluster identity minted at creation. A fresh store
  started with `--join` is **joining**: a token and no id. Restarting a
  store keeps its cluster, id and endpoint; another endpoint is refused
  ("a member keeps its endpoint"); a joining store started as a seed is
  refused.
- A joiner takes the address of the interface that reaches the cluster
  (a connected UDP socket, nothing sent) and the cluster's service port,
  unless `--listen` names its endpoint; a clash says to give `--listen`.
- Discovery: `--discovery-port` (default UDP 17430; 0 turns it off),
  `--discover-on IFACE` (name or address). See below.
- `efsd --help` and `efs-mgmt raft-mkfs --help` describe this. `raft-mkfs`
  is optional: on the seed before formation it proposes at once (refused at
  once with fewer than three) and forms once every proposed store promised;
  its node-table preflight is removed (joining stores are not in tables).

## Mechanism (M4′, M5, M1)

1. Joining stores say HELLO with node id 0 (never entering a node table).
   The seed keeps them as candidates while their HELLOs (every second) keep
   arriving. With two or more fresh candidates and the set unchanged for
   one second, it allocates ids 2..k in endpoint order (IPv4 address, then
   port) and persists the proposal as its own promise (`identity.bin` state
   `promised`).
2. The seed answers a named store with the proposal (`EFS_HELLO_PROMISE`);
   the store persists its promise (refusing another cluster's or another
   proposal than it promised), takes its id and says HELLO again with it and
   the cluster, entering the formation table. A store the proposal does not
   name waits and joins later as an addition.
3. Promised stores with ids 1..3 run group 0 (they know their id and the
   boot voters). The group-0 leader, itself promised, proposes MKFS with the
   proposal once every proposed member is in its table at the proposed id,
   endpoint and token; the MKFS salt is the cluster identity. A lost MKFS is
   proposed again; MKFS keeps the first record. Then the notice and SALT as
   in increment 1.
4. After formation a joining store's HELLO to any member queues its
   addition: MEMBERS proposes `count + 1` for its endpoint and token. Two at
   once propose the same id from one view; one commits, the other is
   `FENCED` and its next HELLO proposes the next id. Once the record names
   the store at its endpoint with its token, the member's answer carries the
   record (and the id); the store persists it before serving. A lost answer
   is repeated by the next HELLO (same id). Pending answers name the
   members' endpoints, so a joiner whose contact goes away asks another.
5. Discovery (`src/server/discovery.c`): every seed, promised or formed node
   answers UDP probes with cluster identity, formed flag, member count,
   service endpoint and build. A joiner without a seed probes the broadcast
   address of each up IPv4 interface computed from that interface's own
   netmask (or only `--discover-on`), three probes over 1.5 s; it ignores
   other builds, refuses when several clusters answer (listing them; an
   explicit seed is required), says that nobody answered and asks again
   every 5 s (never creating a cluster), and says HELLO to an endpoint it
   found, formed members first. Discovery only names peers; the committed
   record decides membership.

## Numbers and formats

- No new Raft log tag, request tag, wire message type or KV kind.
  `EFS_HELLO_PROMISE` = 5 (HELLO answer code, after `_PENDING` = 4).
- `struct efs_msg_hello` gains `flags` (`EFS_HELLO_F_FORMED` = 1); node id
  0 is a joining store. `struct efs_msg_hello_ack` gains `cluster`,
  `formed`, `record_len`, `record[EFS_MEMBERSHIP_MAX_LEN]`; pending answers
  fill `nodes[]` with the members' endpoints.
- Member store `identity.bin` version 3: state joining / formed / seed /
  promised, node id (0 while joining), token, cluster identity, endpoint,
  record (proposal or newest revision). Earlier stores are refused ("of
  another version"): fresh format.
- Discovery datagrams: magic `EFSDISC` version 1; probe
  `[magic:8][1][nonce:8]`; answer `[magic:8][2][nonce:8][cluster:8]
  [count:4][formed:1][port:2][alen:1][addr][blen:1][build]`; anything
  else, another nonce, a zero cluster or port or a trailing byte is ignored.

## Model

`tests/d39_model.py` (commit `e7d9e421d`): the automatic-id mode now forms
the way the code does: a store that promised at id 1..3 hosts group 0, a
promised group-0 member proposes MKFS when every proposed store promised,
the proposal can be lost and is proposed again, every member (the seed
included) learns the record. `growth_automatic_ids_formation_exhaustive`:
3,020 states, 8,990 transitions, 1,006 settled, converges (was 2,201 states
with an atomic seed commit). The other automatic-id scenarios are
unchanged (two automatic-id joiners: 35,137 states, distinct ids 4 and 5;
automatic-id joiner with 2 crashes and 1 wipe: 35,617 states). All 17
growth tests and 8 negative controls pass (13.8 s on xorinox at the tested
commit). No new durable step beyond the model's: the seed's creation (id
1, cluster) is the model's seed store initialization, the promise and the
proposal are the model's `P` states, a joiner learning from the HELLO
answer is the model's `learn`.

## Tests

- `tests/test_membership.c` (`make test`, 119 checks, was 86): joining,
  seed, promised and formed identities; promise rules (another cluster,
  another proposal, a revision as proposal refused; the same again OK;
  restart keeps the promise); learning from promised (only an extension of
  the proposal), from joining (takes the record's id); seed cluster kept
  across restarts; identity v2 refused; a node id disagreeing with the
  record refused; discovery datagram round trip and every malformed answer
  refused; broadcast addresses for /24, /22, /16, loopback /8 (never an
  assumed /24); an unknown interface yields no targets.
- `tests/test_k33_autoid.py` (new, `make test-long`, 30.5 s): no answer
  said and waited, then discovered when a seed appears; formation with ids
  1..3 in endpoint order; interface and port selection (seed on 127.0.0.2,
  joiner without `--listen` takes 127.0.0.1 and the seed's port; a second
  one clashing says to give `--listen`); two concurrent joiners after
  formation get 4 and 5; restarts keep the seed's cluster identity, a
  promised id (joiner SIGKILLed after promising) and member ids; seed down:
  a joiner by discovery is added through another member, a joiner naming the
  down seed says so, waits and joins when it returns; two clusters side by
  side on one discovery port: a joiner without a seed is refused with both
  listed, one naming a seed joins only that cluster, identities differ.
- `tests/test_k33_growth.py` (rewritten for the CLI, 47.9 s): 1–2 nodes
  unready; three form automatically; node 4 added across a proposer exit
  (the joiner asks another member); node 5 exits on receiving the record
  before persisting it (`EFS_TEST_MEMBER_EXIT=before-learn`, now in the
  joining path) and the retried join is the same member 5; seed down, three
  joiners at once through nodes 2, 4 and 5 (5 forwards) get distinct ids 6,
  7, 8; wiped store refused; old payloads exact, new files on added nodes.
- `tests/test_w153_formation.py` (rewritten, 9.5 s): removed flags and
  `EFS_MD_RAFT_N` refused; two refused by `raft-mkfs`; three form; another
  endpoint refused; restart and repeated `raft-mkfs` keep the membership;
  owner lock; five started at once form {1,2,3}/{2,3,4}.
- Converted launchers: `scripts/server.sh` (third argument: a member's
  address or `join`), `scripts/server_processes.py` (parses the positional
  CLI), `tests/faults/harness.py` (node ids from the banners),
  `tests/test_shutdown_*`, `tests/test_retired_root_markers.py`,
  `tests/test_server_processes.py`, `tests/live/server_wrapper_start.py`,
  `tests/{clean_cluster,rdma_first_inode,roll_efsd,valgrind_leaks}.sh`;
  shared helper `tests/k33_daemons.py`. Every test daemon runs with
  `--discovery-port 0` or a private UDP port and probes only `lo`.

## Findings while testing

- **Fixed: a stop signal during the join was lost.** The joining loop
  catches SIGTERM; a signal that arrived just after admission was then
  overwritten by `server.running = 1`, and the node ran on (two
  `test_k33_autoid` runs, captures kept). Now the rest of startup restores
  the default action and serving starts only if no stop was caught.
- **Fixed: a joiner stuck on a dead contact.** A joining store does not
  serve, so it learns its addition from HELLO answers, not notices; when its
  only contact exited after committing it (growth step 3) it waited for
  that contact. Pending answers now name the members, and the joiner asks
  the next one.
- **Fixed (test): restart on another port.** `test_shutdown_stuck_worker`
  restarted a store on a fresh port; a store now keeps its endpoint, so the
  test restarts on the same port and waits only on its own run's log lines
  (first gate iteration on the working tree: 1 FAIL, kept).
- **Open finding (existing code, not this change): placement collapses
  for power-of-two member counts.** `place_wide` starts the stripe at
  `fnv1a(ino) % node_count`. FNV-1a's low bits depend only on the low bits
  of each byte, and inode numbers in one shard step by 4096, so with eight
  members every new file of the growth test started at the same index:
  chunk 0 on {5,7,1}, chunk 1 on {6,8,2}; nodes 3 and 4 got no new
  fragment (`fragments on added nodes {4: 0, 6: 24, 7: 24, 8: 24}`; with
  seven members in increment 1 node 4 got 16). Added capacity is usable but
  unevenly; for alpha 2+1 on more than four nodes the start index needs a
  hash that mixes the low bits (new writes only: recorded placement keeps
  old data readable, M7). Proposed as a follow-up.

## Gate

Host xorinox (16 CPU, Rocky 10), private tree `~/efs-k33b/gate/src` from
`git archive 53080d744`, `TMPDIR` inside it; daemons on loopback ports the
tests choose (never 17xxx), discovery on private UDP ports and `lo` only;
the intel cluster's mount on xorinox was not touched. Topology of the
daemon tests: up to eight efsd processes on 127.0.0.1/127.0.0.2 (TCP, no
RDMA), one store each, `--quota 1G --writers 2`, buffered I/O,
`EFS_GC_DISABLE=1`, 2+1; one or two `efs-fuse` clients in the growth test
(0 B to 3 MiB + 17 B before additions, 24 × 256 KiB after).

```sh
git archive 53080d744 | ssh xorinox 'mkdir -p ~/efs-k33b/gate/src && tar -x -C ~/efs-k33b/gate/src'
ssh xorinox 'cd ~/efs-k33b/gate/src && TMPDIR=~/efs-k33b/gate/tmp EFS_GIT_ID=53080d744 \
  EFS_GIT_BRANCH=claude/devel-k33-inc2-e4299e EFS_GIT_DESCRIBE=53080d744 \
  EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh'
# the K33 commands again in the same tree, to keep their output:
python3 tests/test_k33_autoid.py; python3 tests/test_k33_growth.py
python3 tests/test_w153_formation.py; ./tests/test_membership
python3 tests/test_d39_model.py -k growth
```

Tested commit `53080d744` (parent `e7d9e421d`, the model; base
`2d03d9837`). Build identity: `efsd 53080d744 (branch
claude/devel-k33-inc2-e4299e, built 2026-10-24T02:45:10Z on xorinox)`;
sha256 efsd `d12b15ff443769703b797ed56835f3cb895b7817bd1ed497e7965dc2cb8e7c3d`,
efs-mgmt `faebb2d33fa0f3fe1de0e367b91f3aef00bd4166c800079541a3727732bfe269`,
efs-fuse `e93acb95045dfe3e0b25cdbe9c6bc1a3734a2103f48618c966e054b191c16c9c`.

Result: `TOTAL 436 FAIL 0 NOTRUN 13 STATUS INCOMPLETE (124s) BUILD 19s`,
no `make … Error` line. NOTRUN (host limits, the same 13 as increment 1):
`test_cluster_stop.py` (no `~/git/cluster` on xorinox),
`test_declaration_gate` RDMA path, `test_rdma_xprt` and seven
`test_rdma_segmented` modes (no RDMA device), `test_follower_bound`
stalled-compactor case (needs `EFS_FAULTS=1`), `test_leak_gate.py` (no
valgrind; so `tests/valgrind_leaks.sh`, converted, did not run),
`test_make_test_summary_runner.py` (its NOTRUN fixture). Reruns at the
tested commit: `test_k33_autoid` PASS 30.5 s, `test_k33_growth` PASS
47.9 s, `test_w153_formation` PASS 9.5 s, `test_membership` 119 checks
PASS, model growth 17 passed.

Before the gate, on working trees: `make test` PASS; `test_w153_formation`
PASS at its first run; `test_k33_autoid` failed five times (a wrong log
pattern once, `efs-mgmt status` exiting nonzero for the cluster left
degraded by a stopped member once, the lost stop signal above three times;
two of those captures kept) and passed once after the fixes;
`test_k33_growth` passed both working-tree runs; one full `make test-all`
iteration had the one failure above. On the committed tree every one of
them passed (gate and reruns).

## NOTRUN (parent session)

- Live: three fresh nodes on real hosts forming with the minimal commands;
  additions 3 → 4 → 5+ under load; seed loss; exact payloads including
  packed files; RDMA during formation and addition; orcd-mini; drivers.
- Discovery on a real LAN (bare-metal 10.0.0.x hosts: broadcast on the
  interface's own subnet, several clusters, no answer) and explicit seed
  across subnets (routed). The ORCD fabric's broadcast support is
  unverified; there the explicit seed is the qualified path.

## Remaining

- Live client view refresh (a mounted client sees new members at remount).
- Packed-file payload verification across additions on live hosts.
- The placement finding above (follow-up).
- A proposed store that never returns blocks formation (wipe and restart
  the three); removal is deferred.

## Changes needed outside this repository

`~/git/devops`, `~/git/cluster` (not edited here; the in-repo
`scripts/server.sh` shows the conversion):

- Wipe every store before the first start: identity v3 refuses earlier
  stores.
- `devops/nuc-efs/start.sh` (line 123), `cluster/cluster.sh` (line 125),
  `cluster/orcd-test.py` (line 159): node 1 as `efsd ADDR:PORT STORAGE …`,
  the others as `efsd --join SEED --listen ADDR:PORT STORAGE …`; drop
  `--node-id`, `--addr`, `--port`, `--storage`.
- `cluster/cluster.sh` waits for each node's `listening` before starting
  the next: the first joiner prints it only once the cluster has given it an
  id (formation needs the second joiner), so start every node, then wait
  (or accept `join: waiting|pending` as started). `start.sh` and
  `orcd-test.py` already start all, then wait.
- Node ids are the cluster's: 1 is the seed, the formation's others follow
  in IPv4-address-then-port order, later nodes in commit order. Scripts that
  name logs `efsd-n$id` keep their labels but must read a node's id from its
  banner (`node N listening on`) where it matters.
- `raft-mkfs` steps stay optional (idempotent); their waits for every node
  in `efs-mgmt status` still hold once the joiners promised.
- Discovery: daemons answer on UDP 17430 by default; open it on the LAN
  where discovery is wanted, or pass `--discovery-port 0`.
- `cluster/tests/test_portal_config.py` (line 20) matches an efsd command
  line with `--node-id 1 --port 17432`; `devops/profiling/efs_dd_profile.py`
  (line 62) takes the role from `--node-id`; `devops/nuc-efs/
  test_daemon_ownership.py` (lines 59–75) spawns with `--node-id/--storage/
  --port`: update to the positional form (`server_processes.py` now reads
  the port from `ADDR:PORT` or `--listen` and the storage from the last
  positional argument).

## Raw captures

Kept outside Git on xorinox at `~/efs-artifacts/k33-inc2-20261024/` with
`SHA256SUMS`:

| File | sha256 |
| --- | --- |
| `summary.txt` | `ddd31db7607f3f381909f9252b6b028ea7ddd580e9a514b4723b2c722d4a7f04` |
| `timings.tsv` | `8e4b00a71c5234de8134bbd7a4a1c9b49d464b305b48a43d6ec611e8edae21e7` |
| `runner.log` | `0ec4b8557b5dce2c9b9bc55c5ad1a025de216b057aa4f0a12f1a219c6f0374aa` |
| `build.log` | `da070b9d7237b642b4914a8473d5b9fa6e1849a837eafe431847e610caede618` |
| `efsd-version.txt`, `binaries.sha256` | `9f8693f5…`, `6cf0e667…` |
| `k33_autoid.out` (exact commit) | `97d2ebac54965014a42836404515124ab5de16a8a0a9a672ea51fcbf2e46ed06` |
| `k33_growth.out` (exact commit) | `f7235950a1c32cc6aed0f452b5ee1dd9a198438c43b6a157a5f7d2770c43fcfe` |
| `w153.out` (exact commit) | `aa2e8b7656dd4dfb1c0d2966c6210cd2c3f032f40cf5ea92d1c0973946e913ed` |
| `membership.out` (exact commit) | `fe98924eafc51859651bbb45501f8f24d5b5adba409265f49d445befc4f6eba3` |
| `d39_growth.out` (exact commit) | `81bbc1f0f3cabeb3294e0efc31ac28beb1d7b348965bea60c058beab4d32b961` |
| `iterations/iter1-summary.txt`, `iter1-fail-397-shutdown-stuck-worker.log` (1 FAIL, fixed) | `ace64f29…`, `d0bf9bce…` |
| `iterations/autoid-sigterm-lost-1.out`, `-2.out` (lost stop signal, fixed) | `6dfda5cb…`, `e55aa514…` |
| `iterations/w153-first.out`, `make-test-fast.out` (PASS, working tree) | `2b349f4c…`, `66be1cdf…` |

Full hashes for every file are in `SHA256SUMS` there. The working trees
`~/efs-k33b/src` and `~/efs-k33b/gate` are deleted.

## After the merge

Merged-tree `make test-all` on intel2 (W155, D31 drain, K33 increment 2 and live client view at `910bbeef4`): 442 commands, 0 FAIL, 14 NOTRUN host limits (raw: intel2 `~/efs-artifacts/merge-wave2-20261024/`, `SHA256SUMS` `0d13a146…4ecf`). The first run at `3e01ba95c` failed only `tests/test_k33_client_view.py`, which waited for a joiner log line that increment 2 renamed; fixed in `910bbeef4`.
