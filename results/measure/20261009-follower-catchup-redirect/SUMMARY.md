# SUMMARY — 20261009-follower-catchup-redirect

## Purpose and linked item

Fix the read-serving hole exposed by the preceding transaction fault fixture:
when the client's seed is a follower whose transaction persistence is refused,
an authoritative remote ReadIndex succeeds but local catch-up times out and the
client saw LINK EIO. The host now returns NOT_PRIMARY with the validated leader
hint instead of ever serving the stale local view. Linked item: follower/seed
routing portion of the W66 sequence in
`docs/status/dependency-package-20261009.md` (evidence link at line 661);
follow-up to `results/measure/20261009-transaction-durability`
(seed-routing-fault.txt). Run added by commit `074b65123` ("fix(raft): redirect
reads when follower catch-up times out").

## Verdict

PASS. Live fault fixture passed every gate (node listening, follower baseline,
persistence-refusal lag hold, catch-up after fault release, cold linked
namespace, LINK/rename through leader route, full POSIX and physical
reclamation). Native extracted-policy test
(`tests/test_follower_read_redirect.py`) passes. Closes the observed seed
catch-up timeout case only.

## Source commit and build identity

- Code commit: `074b65123` (adding commit of this run).
- Fixture binary hashes recorded in manifest.json: efsd
  `44b6c52b86d87bc3…`, efs-fuse `f4840d6e21f7a306…`, efs-mgmt
  `4d0b2af759ca429d…`; source tree sha256 `0b0fe46507f0109c…`, test script
  sha256 `8081aad3d42b3a4e…`. Fixture server binary:
  `/data1/efs/evidence/txn-apply-fault-src-20261009/efsd`.
- No deployed-*.txt file; installed services untouched by this fixture.

## Environment

AMD Linux host, four daemons on one host (private fixture roots
`gc-direct-o9d7vxee` n1/n2 and `gc-direct-l7x7oyfd` n3/n4 under
`/data1/efs/evidence/lease-retention-fixtures/`), direct I/O, TCP transport, port 30240,
pressure off, `transaction_fault_seed` and `transaction_apply_retry` fault
gates enabled (manifest.json). Mounts `/data1/efs/mnt` and `/data1/efs/mnt2`.

## Method

1. Native unit run including the extracted-policy follower-redirect test
   (unit.txt) and the code regression recipes (regressions.txt).
2. Live fixture: start four private daemons, let the follower baseline catch
   up, then refuse transaction persistence on the owned private seed (node 1)
   so it rejoins as a follower that cannot apply its read index.
3. Verify LINK/rename complete through the leader route while node 1 holds a
   15–18-entry apply lag; release the fault and confirm exact apply retries
   catch it up.
4. Cold restart and verify exact linked bytes and nlink=2; run full POSIX and
   physical reclamation in the same fixture; unmount and retire both clients.

## Results

| Gate | Result |
|---|---|
| Follower-redirect policy unit test | PASS |
| Code regression recipes | PASS |
| Follower persistence refusal, held apply index | PASS (lag 15–18 entries) |
| LINK/rename via leader route during fault | PASS |
| Catch-up after fault release, cold restart byte/nlink check | PASS (exact bytes, nlink 2) |
| Member rejoin reclamation + queue drain | PASS |
| Full POSIX and physical GC/restart/quota/late-PUT fences | PASS |

## Limitations and remaining gates

- Single AMD host with four daemons; injected persistence/process recovery,
  not hardware power-loss proof.
- Closes only the observed seed catch-up timeout case — not all W66 placement,
  scaling or partition availability gates.

## Evidence index

| File | sha256 (16) |
|---|---|
| README.md | 2153b02dd17fa295 |
| manifest.json | 9ab52173c6574fbd |
| fault-proof.json | ff1da8ce90b5f151 |
| live.txt | e99f244fe5c20f36 |
| regressions.txt | 1b1f4e44b2b18a5b |
| unit.txt | f4b710f3e4cd919b |
