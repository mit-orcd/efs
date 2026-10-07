# Specification/public-path review — Oct 7, round 3

[Current queue](../status/README.md) · [Public-path matrix](../status/spec-implementation.md)

Review started 15:00 UTC with a 15-minute limit, at HEAD `ddc677ae` plus
existing working changes. This is documentation/source/evidence review:
no production source changes, runtime test execution or cluster mutations.
A source gap does not establish an observed incident's cause or a current
running binary's behavior. Prior healthy NUC acceptance remains build-scoped.

## New canonical identities

| ID | Inspected source | Finding and limit |
| --- | --- | --- |
| [W71](../backlog/work-items.md#w71) | `store.c:server_write_fragment_to_path`, `shard_io_thread`, `finish_shard_write`; `handler.c` PUT; FUSE fsync | Production fragment success has no persistence barrier; benchmark sync lives in separately compiled objects. No power-loss experiment was run; device/filesystem guarantees require acceptance, not assumptions. |
| [W72](../backlog/work-items.md#w72) | `inode_rpc.c` opid seed/HOLD/FLOCK/append; `raft_host.c:host_sess_gate`; staged publication admission; mgmt session coordinator | Real metadata session primitives exist; mount-wide establishment and dependent public paths are not connected. Local epoch 1 is retry identity, not a completed revocation lifecycle. |
| [W73](../backlog/work-items.md#w73) | Both FUSE admitted-write callbacks and low-level write replies | No explicit O_SYNC/O_DSYNC publication branch; real kernel-follow-up fsync sequencing was not traced, so early syscall success is not claimed as a reproduction. |
| [W74](../backlog/work-items.md#w74) | `common.h`, `erasure.h`, `publication.c`, `meta_apply.c:evidence_ok`, HEAL_STATUS | Fixed 2+1 and K2F1-only staged validation; no live-fragment repair/profile-cutover owner found. Two-survivor reads, GC and metadata recovery are not repaired protection. |
| [W75](../backlog/work-items.md#w75) | Payload hashes in write.c; store/read checksum path; handler GET; `get_one_reply`, `efs_client_get_fragment` | Missing stored checksum can be replaced by a fresh hash; common parallel reads ignore returned hashes and bypass EFS_READ_VERIFY. No new corruption experiment was run. |
| [W76](../backlog/work-items.md#w76) | `client_internal.h:EFS_OPID_INFLIGHT`, `inode_rpc.c:opid_begin/opid_suffix` and namespace callers | 512-slot exhaustion omits retry identity and still sends mutations. Default-mount concurrency reachability remains unverified. |
| [W77](../backlog/work-items.md#w77) | Per-group `host_pub_batch`, per-chunk PUBLICATION, `efs_client_read_refs` | Request-wide atomic publication and multi-chunk observation need a dedicated gate; no torn-read experiment was run, and above-FUSE-request atomicity remains explicitly unclaimed. |

Each has one active queue row, detailed evidence, next action and gate.
They add no priority override, repair implementation or accepted design change.
W71 is distinct from D28's pending-client-death salvage choice; W73 flag
handling alone would not fix target persistence. W72 retains implemented
D25 admission instead of relabeling it absent. W42's quota/debt concern
remains separately indexed. These findings do not diagnose the GC incident.

## Decision register and verification cleanup

- D25–D27 full accepted contracts/gates moved to the active
  [contract detail](../backlog/decision-contracts.md), not the archive. The
  register is shorter and explicitly links them; constraints remain binding.
- D28–D30 stay unresolved asks. D29/D30 references to missing rows N/O now
  point to the historical ordering record rather than a nonexistent table.
- D10's file-cap removal was later revised by D12; D12's L1-rewriting
  backstop was revised by D13. Those supersession links are explicit.
- Source checks found the 512 MiB snapshot trigger, import slicing,
  first-PUT sentinel, nonwaiting apply pressure path, byte-triggered
  compaction and L0-only file-cap merge. This is implementation inspection,
  not fresh acceptance of D4/D5/D7/D9–D13 on hardware.
- Sep 2 and Step 9–10.5c verification checkpoints are
  [archived intact](verification-checkpoints-20261007.md). Active coverage
  no longer claims production still uses the in-memory table or lacks opids.
- Simulation models, process restart and physical power loss are distinct.
  Test-file existence, successful remount and logical simulation are not
  blanket durability/fault acceptance. Real hardware gates also check correctness.
- Cluster RPC metadata benchmarks (`<seed:port> --meta`) and local
  engine metadata benchmarks (`--bench meta`) are explicitly distinguished.
- Performance requirements and fcstor baseline configurations are labeled
  design requirements/dated evidence rather than current universal deployment facts.

## Remaining scope estimate

Approximately **35% of review effort remains (±10 percentage points)**:
complete operator/quickstart command and restart/stop checks, re-audit remaining
product-surface claims and performance evidence/configuration links, and finish
cross-page/identifier/closure consistency. Remaining review is distinct from
implementing the newly recorded findings; those fixes can be substantially larger.
Some earlier accepted-decision/hardware gates still need focused verification.
