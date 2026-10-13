# Strict segmented RDMA — AMD integration qualification

Historical isolated checkpoint. Subsequent installed strict-RDMA deployment and
expanded gates are in the [installed AMD ledger](rdma-installed-amd-20261013.md).

Branch: `devel-rdma`. Final runtime source:
`a32567d7ef1153b19182cf4f028ac6027bfe39bd`.
[Agreed contract](rdma-segmented-messages-plan-20261013.md),
[foundation checkpoint](rdma-implementation-20261013.md).

**Integrated / AMD affected gates PASS on finala32567d7.**
Earlier39d93093 source passed full common-library ASan/UBSan Soft-RoCE normal
and fault transport tests, core216/216, peer67/67, and the same-source TCP
controls. All332 upgraded fixture TCP sockets measured zero post-setup payload.
A final audit found repeated RDMA_SETUP could bypass application channel
selection through the handler's raw bootstrap reply. Finala32567d7 rejects
TCP/RDMA setup reentry before any raw send; production-block regression and
its guard-removal negative control PASS. Final filesystem reruns are recorded
below, without borrowing earlier verdicts for a changed handler.

No push, tag, devel merge, installed AMD deployment or hardware-IB deployment.
AMD's original four-node96f40602 files/TCP/direct cluster remains running,
commands/packing/containers/expiry disabled. Test fixtures use separate ports,
stores, processes and mounts and are stopped after qualification.

## Integrated behavior

- `rdma.c` negotiates exact72-byte segmented-v1 setup for IB or RoCE, distinct
  from address version, validates limits/actual peer receives and rejects
  legacy setup. auto fallback is only a setup decision. Both setup directions
  use raw TCP bootstrap; after success every supported application frame is
  segmented over RDMA. Unexpected application TCP retires the connection.
- Physical buffers remain84KiB;32-byte DATA/CREDIT envelopes leave85,984bytes
  per DATA frame. Logical maximum128MiB includes five-byte application framing.
  Per-connection assembly reservation256MiB, process-global512MiB; completed
  FIFO maximumfour. Maximum fragment64MiB plus PUT/GET headers fits; Raft AE
  commands are1MiB byte-capped batches and snapshots4MiB chunks with bounded
  wire headers (`raft.h`, `wire.h`). Larger general3GiB syntax is not allocated
  or redirected to TCP: unsupported logical lengths fail the connection.
- A single checked-out owner advances DATA/CREDIT and send CQEs, copying each
  received segment then reposting it before returning cumulative credit. DATA
  uses peer-receives minus one, a dedicated send slot handles/coalesces credit
  records, and SEND completion never replenishes DATA credit. Slot exhaustion
  returns to connection progress instead of blocking inside the selector.
  Registry poller retains its generation checking and only publishes CQEs;
  allocation/progress/handlers never run under its registry lock.
- Whole sends preserve one original TLS deadline, capped by existing5s send
  wait; assembly/receive uses the original operation budget. All sources copy
  segment-by-segment into registered pool slots; callers can reuse p1/p2/TLS
  buffers on return. W39 two-SGE and direct registered reply fill are disabled
  initially; generic GET replies remain RDMA. No full send bounce allocation.
- All generic/specialized receive and readiness APIs acquire complete logical
  messages. CREDIT and partial assemblies are not replies. Generic receive
  transfers/moves the existing allocation into normal caller free ownership;
  receive-into currently copies from one bounded assembly (not direct assembly
  into caller destinations). This is a correctness baseline, not copy-cost
  performance acceptance. Handler holds its completed view through synchronous
  dispatch; a corrupt subsequent assembly cannot free that borrowed view.
- Raft application pipeline is bounded by negotiated whole-message capacity,
  independent of physical DATA credits. FIFO complete replies are counted once;
  transport CQEs/credits do not alter durable consensus acknowledgments.
- Per-connection counters distinguish DATA frames/bytes, logical messages/bytes,
  controls, credit stalls and assembly peak. TCP_INFO records attributable
  bootstrap bytes and post-setup TCP payload; received FIN octet is excluded
  in CLOSE_WAIT. Unmeasurable sockets are explicit (`tcp_measured=0`); AF_UNIX
  transport regression sockets are not TCP-byte proof. Full filesystem proof
  uses actual TCP bootstrap sockets, including client/server and peer links.

## Environment and gate ledger

AMD-395, Ubuntu, native build on same host; Soft-RoCE `rxe0/1 ACTIVE LINK_UP`,
GID index1, backing netdevwlp194s0, same-host RC QPs (not native cross-host IB).
Four private daemons127.0.0.1:27432–27435, files engine/direct I/O,4G quota each,
two FUSE mounts under `/home/efs/efs-rdma/gate-{rdma,tcp}-*`.
Every fixture inherits `EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0
EFS_EXPIRY=0 EFS_REPAIR_EXPERIMENT=0`; containers OFF via files engine.
Core is serial, default15s minimum and original longer per-case budgets; peer
suite uses original defaults. No timeout or credit-window tuning.

| Gate | Final verdict |
| --- | --- |
| Production setup reentry / guard-removal negative control | PASS |
| Pure codec/budget/credit ownership and wrapper deadline/slot checks | PASS on unchanged transport inputs |
| Final Soft-RoCE core POSIX | PASS216/216,46.0s, no skips/notrun |
| Final Soft-RoCE peer POSIX | PASS67/67,11.7s |
| Final same-binary TCP core/peer control | PASS216/216(47.5s),67/67(10.5s) |
| Final actual socket post-setup TCP-byte proof | PASS333/333 measured upgraded sockets,zero post-setup TCP;19 segmented-send connections |
| Full-library ASan/UBSan Soft-RoCE | PASS finala325 normal,interrupt,TCP violation,50ms deadline,68-message smoke; leak detection ON |
| Concurrent seven-client, cold persistence, supported max-size filesystem/metadata/snapshot matrix | NOTRUN |
| Native cross-host IB, all required ORCD-specific gates | NOTRUN |

Transport regression covers both endpoints sending8MiB before reading a reply,
actual asymmetric2/7 receive pools, physical-envelope boundary sizes,128KiB,
1MiB,16MiB and64MiB large reply to a4byte request, exact bytes, receive-into
canaries/status-only errors, immediate source reuse and zero reservations after
teardown. Fault modes interrupt a partial assembly/credit wait, send an illegal
TCP byte after setup, and withhold receiver progress under a50ms original TLS
budget (must fail before200ms, not restart5s for each segment). Device skips
remain NOTRUN. The pure codec tests cover malformed extents/IDs/FINAL, queue,
per-connection/global/allocation refusal, concurrent reservations and credit
replay/inflation; these do not substitute for all live QP fault permutations.

Two initial `/data1/efs/rdma-gates` fixtures failed before POSIX at FUSE mount.
Kernel audit proved AppArmor denied that mount path. Directory permissions did
not fix it. Moving the isolated fixture to the already-permitted home path
worked without changing host security policy. Failed raw evidence remains
outside Git. Native build also retains preexisting query_disk misleading-
indentation and expiry array-parameter warnings, outside this transport scope.

## Exact reproduction

Export finala32567d7 to an isolated AMD source directory. Build:

```sh
make -B -j8 EFS_GIT_ID=a32567d7ef11 EFS_GIT_DESCRIBE=a32567d7ef11 EFS_GIT_BRANCH=devel-rdma efsd efs-fuse efs-mgmt tests/test_rdma_xprt tests/test_rdma_segmented tests/test_wire
make test-rdma-message
python3 tests/test_rdma_reply_budget.py
python3 tests/test_rdma_setup_reentry.py
./tests/test_wire
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=rxe0 EFS_RDMA_GID_INDEX=1
for mode in normal interrupt tcp-violation deadline; do
  timeout 45s tests/test_rdma_segmented "$mode"
done
mkdir -p /home/efs/efs-rdma
for mode in rdma tcp; do
  EFS_TRANSPORT=$mode EFS_GATE_ROOT=/home/efs/efs-rdma bash tests/live/rdma_posix_gates.sh
 done
```

Harness owns private fixtures, explicitly selectsfiles/disabled flags, retries
fresh mkfs only within startup budget, runs core216 and peer67, then cleanly
unmounts and stops its four daemons. Do not point it at installed stores. Retain
its external logs for traffic proof. Do not label a skipped preflight PASS.

Sanitizer build in a separate source workspace, every libefs object rebuilt:

```sh
make -B -j8 CFLAGS='-O1 -g -fno-omit-frame-pointer -std=c99 -Wall -Wextra -D_GNU_SOURCE -fsanitize=address,undefined' LDFLAGS='-lpthread -lm -ldl -libverbs -fsanitize=address,undefined' tests/test_rdma_xprt tests/test_rdma_segmented
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=rxe0 EFS_RDMA_GID_INDEX=1
for mode in normal interrupt tcp-violation deadline; do timeout 45s tests/test_rdma_segmented "$mode"; done
timeout 45s tests/test_rdma_xprt
```

Test-only sanitizer compilation uses default build markerunknown; exact source
input hashes govern it, not a deployment claim. Handler is native-tested; these
sanitizer binaries instrument the full common library, not running server/FUSE
handlers. Remove test binaries only when no owned processes remain.

TCP proof after clean fixture teardown: collect `RDMA counters` lines from all
four `n*.log` and both `efs-fuse-*.log`; require nonempty collection, every line
`tcp_measured=1`, `post_setup_tcp=0/0` and `violations=0`. Explicit TCP management
connections without RDMA setup and unrelated host TCP are outside this claim.
Repeat without the setup-reentry guard only in an isolated source copy: the
production-block regression must fail its no-send assertion. Do not deploy that
negative control. Codec offset-validation removal must fail its overlap oracle.

Raw output/binaries outside Git under `/data1/efs/rdma-*`, and per-fixture
`/home/efs/efs-rdma/gate-*`. Final native and full-library sanitizer workspaces
verify all871 tracked build/test inputs. Input-manifest SHA256:
`075a6aabd7d7dfd570b669d85fb14d99bf2ace290f2e5ee2f9d40299ca0feed3`.
Manifest generation: sorted `git ls-files Makefile src include deps scripts tests`,
SHA256 each file, then `sha256sum -c` in each source tree. Historical results
are excluded from this input inventory and these affected gates.

Native build2026-10-13T21:31Z, markera32567d7ef11/branchdevel-rdma; binary SHA256:

- efsd: `1d5541555a9fec33fcb6e2bdfc552acdaf6bf16c29d089117ee03706943f2e38`
- efs-fuse: `c5d82d4df1054eb14257b9088570ee014eabe9748a1f5c83ad5d5f0abfc7f572`
- efs-mgmt: `e674b5418f7a42ef4a8c96e863f8ba09e97883fd2fd22442cc69277b5e944635`

Final RDMA fixture `/home/efs/efs-rdma/gate-rdma-isINNK`, TCP fixture
`/home/efs/efs-rdma/gate-tcp-M0PK0B`; both cleanly stopped/unmounted, original
installed four daemons/two clients remain. All333 upgraded TCP sockets measured
zero post-setup payload/violations;19 connections have more DATA frames sent
than complete application messages (example37,283frames/33,802messages), proving
actual multi-segment filesystem/peer traffic, not only an isolated echo.

Result TSV SHA256 (external, not committed):

- RDMA core: `87cd4177f337a4b8e310660fba8a4eb917541e21a80e5159a712d76675df13ce`
- RDMA peer: `8bd99dc986e6cf844f0383a3c412836246797f9c674691c61df0e84f06ae1fc8`
- TCP core: `2aeec7a62cfea8f96a3f02b8985ae0045bc68bd448b2cde2bee587b6aea3bc14`
- TCP peer: `0aed1c774617f6fada74d3598bb61dce6392bed0052a0da850ac2110eafffeb3`

Final native fault logs `/data1/efs/rdma-a325-fault-*.log`; sanitized source
`/data1/efs/rdma-san-a325`, logs `/data1/efs/rdma-a325-san-*.log`. No sanitizer
findings. Mac generated reentry-stub first launch once exceeded5s; identical
rerun and Linux production-stub test passed, without changing its timeout.

This closes the requested initial implementation/AMD affected test checkpoint,
not the full agreed RDMA production acceptance matrix. Remaining live malformed
credit/envelope injection, repeated same-process reconnect/generation churn,
large metadata/snapshot/cold durability and loaded concurrency gates must be
qualified before rollout. Generic receive-into assembly copying and disabled
registered reply/W39 fast paths are explicit implementation tradeoffs. No
speedup conclusion from these correctness test durations.

