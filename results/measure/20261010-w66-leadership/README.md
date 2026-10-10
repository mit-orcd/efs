# W66 staged metadata leader distribution — Oct 10, 2026

AMD single host, private stores, direct-I/O/TCP. Candidate is opt-in with
`EFS_METADATA_BALANCE=1`; default remains off. Installed stores/topology were
preserved. No D36 or independent-host/power-loss claim.

## Gates

- [Engine handoff and fatal sanitizers](engine/README.md): source fenced before
  trigger; normal quorum election, stable membership, tail/term checks,
  target campaign/pressure fences and recent successful replication response.
  An old idle matched tail cannot initiate handoff to an unavailable peer.
- Production host policy extraction: two-second grace, actual sparse voter
  set, lag refusal, thirty-second backoff retained across source re-election,
  membership-change reset. `make test` runs this gate; [native log](native.log).
- [Four-voter results](four-voter/results.json): 800 reads, 36 exact-inode
  recovery probes. Both groups explicitly reconfigured to 0xf through
  production raft-change. Preferred leaders on nodes 1/2 before load, after
  both rejoin cases and after cold restart.
- [Legacy results](legacy/results.json): same 800/36 gates, production default
  memberships 0x7/0xe, preferred leaders on nodes 1/3 at those boundaries.
- [Prolonged loss](outage/results.json): each preferred leader absent for 35
  seconds. 420 read probes pass without retries at the harness call level,
  identity mismatch, observed leader/term churn or failure. Available groups
  may temporarily share a surviving leader. Rejoin/cold placement also passes.

Read timings include management CLI startup. Load consists of two sequential
create/unlink writers. The outage run has only ten timing samples per cell;
its purpose is availability and stability. There is no controlled throughput
comparison, multi-host failure-domain proof or 1000-active-client claim.

## Reproduce

Source `/data1/efs/merged-storage-src-20261010`; binaries have identity `unknown`
because the private source has no `.git`. This does not close exact-build-ID
release gates. Baseline parent is `6c1a93b9`; engine commit is `bab3d1d7`.

```
python3 tests/measure/w66_readindex_latency.py --source "$PWD" --server "$PWD/efsd" --work UNIQUE_PRIVATE_DIR --mode direct --samples 100 --port UNUSED_PORT --four-voters --balance --recovery
# Legacy: omit --four-voters. Prolonged loss: --samples 10 --outage-seconds 35.
make test
```

Tested host SHA256:

```
4030b188187b8ccb0dda4cf95944296041ca51b9fa8b82f5e84905a255a9430a  src/server/raft_host.c
4b7dc3b534d000be6b04fe94fcd17710b896624a13fe8a3aa49fc283be8b8b15  tests/test_metadata_balance.py
```

## Full functional gate and correction

The initial staged full run passed core POSIX 212/212 but peer 63/64 from an
unresolved OPEN BUSY during concurrent append. No handoff occurred at the
failure boundary. [The retained OPEN correction](open-fixed/README.md), commit
`a7beb88c`, passes the negative-control/ownership/deadline tests, canonical
native tests, twenty append races (16,000 records), full peer POSIX 64/64,
exact cold bytes and drained GC queues. A separate physical scan finds zero
fragment/checksum files. An intervening harness-only cleanup failure is kept
as failed evidence. Installed direct/TCP service is restored and no owned
private process remains.

## Still open

This distributes the existing two groups. W66 still needs durable commit proof
and verified recovery for D36, persisted versioned routing, migration, more
independent groups and controlled useful-capacity/hot-directory measurements.
The post-two-loss availability contract awaits the user's answer. Four-voter
membership and balancing are not enabled on installed stores by these tests.
