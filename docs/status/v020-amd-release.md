# v0.2.0-pre-alpha — AMD acceptance

`devel-roce-rdma` merged into `devel` in `952b57d5`. Conflicts retained the
newer split-LOOKUP correctness fix and completed TCP evidence; temporary
fault traces and obsolete open states were not restored. RDMA adds versioned
RoCE GID routing and RXE CQ capacity admission. The branch also contains its
existing storage benchmark prototype snapshot; this does not activate a new
storage-engine format or D25 publication. D25 remains staged.

Clean merged source `2ba0d7cc` builds and passes full Linux `make test` on
AMD.349 tracked build/test files match the deployed sources exactly.

| Storage | Transport | POSIX | Two-client | Cold durability |
| --- | --- | --- | --- | --- |
| Direct | RDMA |216 pass /0 fail /1 skip |64/64 |26 prepare +26 verify |
| Direct | TCP |216 pass /0 fail /1 skip |64/64 |26 prepare +26 verify |
| Buffered | RDMA |216 pass /0 fail /1 skip |64/64 |26 prepare +26 verify |
| Buffered | TCP |216 pass /0 fail /1 skip |64/64 |26 prepare +26 verify |

The skip is unsupported writable mmap. All four daemons and both clients
were checked for the actual mode/transport; all six RDMA processes hold
uverbs handles and TCP processes do not. Separate RDMA transport probe
passes67 RDMA frames and its intentional oversized TCP control. AMD uses
RXE software RoCE on rxe0/GID1; these tests do not establish hardware-offload,
cross-host, native-IB or mixed-version acceptance.

Durability remounts preserve the selected transport; the former forced-TCP
driver is repaired in devops commit `e0fcc1f`. No store reformat, forced
discard or durability relaxation was used. The Python symlink hard-link test
now explicitly requests no-follow behavior; no production link repair was
needed.

[Raw logs, binary/source hashes, process proofs and TSV verdicts](../../results/measure/20261008-v020-amd/README.md).
The release commit adds documentation/evidence to the tested code. Annotated
tag `v0.2.0-pre-alpha` is prepared only after all four configurations pass.
Existing roadmap, scale and RDMA stress gates remain as separately recorded.
