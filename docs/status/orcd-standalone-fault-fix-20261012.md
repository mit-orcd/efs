# Isolated ORCD standalone fault and repair qualification — October 12

**Follow-up:** [Mandatory default suite qualification](orcd-default-repair-20261012.md) supersedes the default-GAP behavior described here. The results below retain their original experimental scope.

The user's subsequent explicit request to fix the standalone fault suite is
handled on `fix-orcd-fault-repair-rdma`, separately from release qualification.
Implementation is frozen at `ed749cab212768137a32ee8d3a2b80fa79744215`.
The release worktree/ref and installed ORCD source/binaries are not modified.

## Change and scope

The harness accepts `--transport tcp|rdma`, `--rdma-device` and explicit
`--repair-experiment`. Native RDMA requires live uverbs FDs and a successful
upgrade log from each current daemon/client incarnation, including cold restarts;
a stale log or silent TCP fallback cannot satisfy it. Journals and transport
logs survive successful cleanup, and config records exact replay argv.

Repair stays default-off. Explicit setup activates only owned, disposable,
single-root FILES fixtures. The manual coordinator149 accepts a selector, never
caller data or receipts. It captures current lane leader/term and authoritative
placement/checksums, verifies both surviving full fragments, XOR reconstructs
and checks the missing role, commits ADMIT, obtains the actual target's durable
DATA receipt, commits DATA, obtains/commits the durable terminal SEAL, and retires
the job. Current FileID/source, target incarnation, committed-entry term and
receipt-context fences remain. Public generic journal mutation55 stays refused.

Imported prerequisites are `b75523c3`, `92e21f0c`, `737ef0b6` and `6faf3ebe`,
with the documentation conflict resolved to the prerequisite design. Their
experimental fragment and whole-inode GC debt guards prevent deletion/ACK while
repair debt is active. The normal/default-off GC path remains scan-free. One
worker per daemon bounds concurrent repair memory and connections.

A cold TCP fixture exposed unavailable source connections before ADMIT: the
shared peer pool retained its ten-second down cooldown after a source daemon had
restarted. The old-pool diagnostic control confirms a refused node4 connection
with active cooldown; one earlier diagnostic control passed and a subsequent
control failed, so the failure is timing-dependent. The worker now uses fresh,
owned peer connections, avoiding that shared negative cache. No retry budget or
fault-case deadline was increased. Three consecutive corrected TCP fixtures
returned 23 PASS/exit0 before final frozen-source qualification.

## Actual repaired-role oracle

For each role0/1/2 the real fixture publishes a full ordinary base chunk, deletes
one physical fragment, proves an exact fresh degraded read does not recreate it,
invokes the actual coordinator, checks the restored payload and embedded digest,
cold restarts all four daemons and both clients, deletes another original role,
and verifies exact application bytes from a fresh peer. The simulator separately
repairs each role using identity-bound authoritative hashes and verifies exact
bytes after a second corruption; it does not claim daemon durability or quorum
receipts.

The worker regression executes the actual worker with transport/authority
adapters and real hashes/receipt codecs. It covers default refusal, null inputs,
all roles, poisoned/truncated source before admission, lost target response,
wrong receipt context and failed DATA commit, with no terminal ACK/retirement
on failure and every owned connection destroyed. The actual prepare/record
adapter covers fresh capture, follower refusal and old-term pending-job refusal.
The live wire fixture includes default-off149 and malformed/build-mismatched
request refusal, alongside activation/incarnation/cold-start refusal gates.

## Qualification ledger

Final qualification on exact `ed749cab` **PASS**:

| Gate | Result | Evidence |
|---|---|---|
| Complete fault-enabled native build/test | exit0 | [log](../../results/measure/20261012-orcd-standalone-fault-fix/remote/efs-qualified-v3-fault-native.log) |
| Complete normal native build/test | exit0 | [log](../../results/measure/20261012-orcd-standalone-fault-fix/remote/efs-qualified-v3-normal-native.log) |
| Normal-build native RDMA/direct IO standalone | 23 PASS, exit0; all three repair roles | [results](../../results/measure/20261012-orcd-standalone-fault-fix/remote/efs-qualified-v3-rdma-ed749cab/results.jsonl) |
| Normal-build TCP/direct IO standalone | 23 PASS, exit0; all three repair roles | [results](../../results/measure/20261012-orcd-standalone-fault-fix/remote/efs-qualified-v3-tcp-ed749cab/results.jsonl) |
| Actual worker ASan/UBSan and leak detection | PASS | [log](../../results/measure/20261012-orcd-standalone-fault-fix/remote/efs-qualified-v3-worker-asan.log) |
| Default-off/manual RPC/incarnation boundary | PASS; separate from real repair proof | [result](../../results/measure/20261012-orcd-standalone-fault-fix/remote/efs-qualified-v3-wire-ed749cab/result.json) |
| Post-test source fingerprint | 1,059 source inputs match frozen commit | [proof](../../results/measure/20261012-orcd-standalone-fault-fix/controller/source-proof.json) |
| Owned private processes/mounts retired | PASS | [proof](../../results/measure/20261012-orcd-standalone-fault-fix/controller/cleanup.json) |
| Installed ORCD deployment preserved | same eleven PIDs, binary hashes, arguments, UID and8b7391b3 source; repair disabled | [proof](../../results/measure/20261012-orcd-standalone-fault-fix/controller/main-preservation.json) |

The three-role protection verdict describes the verified repaired state **before**
the second injected loss; the subsequent two-role read is an independent
witness that the restored copy works after cold restart. Simulator PASS is not
borrowed as physical durability acceptance.

[Machine summary](../../results/measure/20261012-orcd-standalone-fault-fix/summary.json),
[artifact hashes](../../results/measure/20261012-orcd-standalone-fault-fix/artifact-sha256.json),
and [exact source manifest](../../results/measure/20261012-orcd-standalone-fault-fix/controller/qualified-inputs.json)
retain 543 hashed evidence/controller files. Historical documentation evidence
was copied from the actual existing8b source and overlaid with committed added
artifacts to satisfy link checks; it is not counted as new acceptance. Both
snapshot setup failures remain distinct from code and live-test failures.

Reproduce on an isolated machine/source copy with an active HCA and FUSE:

```sh
make tests/faults/fault_sim tests/faults/repair_request efsd efs-fuse efs-mgmt
EFS_FAULT_BIN=/path/to/exact/build bash tests/faults/run_faults.sh /tmp/new-owned-fault-fixture \
    --integrate --mode direct --transport rdma --rdma-device mlx5_2 --repair-experiment
```

Use a fresh output path. Substitute `--transport tcp` for the other qualified
transport. Do not activate existing mounted stores or transfer acceptance to
another source. The private compiler/runtime, exact snapshot construction,
initial failures, corrected setup and final commands are retained in the
controller files.

Retained precursor evidence includes:

- Original transport-only run: 19 PASS, 2 genuine repair GAPs over RDMA/direct IO.
- Initial short-write repair fixture: refused delta object, FAIL; the receiver
  supports ordinary base objects, and this failure is not relabeled PASS.
- Pooled-worker TCP failures: source connection unavailable before admission.
- One old-pool control PASS and a later FAIL with explicit cooldown/refusal.
- Corrected fresh-connection TCP: three full private fixtures, each23 PASS.
- Initial frozen-source snapshot: native ZIP fixtures rejected1970 timestamps
  introduced by the archive builder. Corrected metadata and a new fixture retain
  identical file bytes; this setup failure is preserved separately.
- The next source snapshot omitted historical evidence required by the doc link
  gate. Actual committed evidence was restored before final native acceptance.

## Remaining capability limits

This is bounded experimental ordinary **missing base-role**, same-incarnation,
same-target repair. Delta spans, spare relocation,
empty replacement stores, damaged existing-role replacement, automatic repair,
interrupted-job leader takeover and comprehensive distributed pending-GC/late
writer races remain unqualified. Errors retain debt; explicit debt cancellation
and takeover orchestration are not implemented by this driver. Activated roots
cannot safely be restarted default-off by deleting the activation/identity
markers. The private driver is not an installed user administration interface.

Default standalone integration still reports its named repair GAP. The explicit
experimental command is the new restored-role acceptance path. This does not
close broad production repair/recovery work, existing query failures,
or extend release scheduling scope.
