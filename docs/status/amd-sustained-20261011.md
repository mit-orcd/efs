# AMD sustained tiny/large service gate — Oct 11

W94's finite CREATE adoption A/B and full normal/fault/direct/buffered gates
pass on the [captured integration](amd-integration-20261011.md). This follow-up
targets sustained service, without claiming full production session/PUT-ticket
integration or selecting a new storage format.

The existing private four-node TCP fixture runs direct then buffered, 600
seconds each. Eight tiny workers rotate 64 names per worker, writing 512-byte
generation-specific bodies. Every acknowledged generation is byte-checked
before unlink; names are reused only after unlink. Four large writers overlap
16-MiB rings of 1-MiB writes, fsync every four writes and at completion. Thirty-
second windows record CREATE/close/delete completions and latency, with process
CPU ticks and RSS samples. Explicit per-daemon quota is 4 GiB; installed data
and daemons are retained. This measures the accepted combined-adoption build;
it is not a new same-source A/B throughput claim.

After each timed interval, the private servers crash and restart. All 512 tiny
survivors and all four large files must match exact expected bytes. Unlink must
then produce both empty GC queues and empty physical fragment inventories.
The harness drains/restores the two installed mounts with TCP and commands off.
No RDMA, D25 activation, production packing or pushes are part of this gate.

[Reproducible runner](../../results/measure/20261011-amd-sustained/run.sh).
Acceptance and remaining blockers will be recorded after the actual verdicts.
