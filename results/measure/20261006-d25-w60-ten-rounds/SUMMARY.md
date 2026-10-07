# D25 / W60 ten-round checkpoint — Oct 6 2026

Code commits: `73ce8aaa` (W60), `5dc5e9e0` (staged D25 ACK ownership).
Testing used the four-node NUC. `/data2/efs` resolves onto `/home`, which had
827 GiB free; `/data1` had 730 GiB free at admission. No storage wipe occurred.

## Ten implementation / regression rounds

1. Add atomic speculative body admission at half the normal budget; verify
   demand allocations can spend the remaining half without raising limits.
2. Apply the same admission policy to reproducible read-cache copies; preserve
   stolen prefetch body ownership and existing pending/reply-pin protections.
3. Add demand scratch recovery: trim unpinned clean bodies, then at most twenty
   1 ms waits outside cache/index locks. Test fast, immediate recovery, delayed
   release and exhausted paths against the production function.
4. Exercise sixteen simultaneous speculative allocators, request credit and
   drain-context exclusion. Budget bounds and actual ownership stay intact.
5. Reproduce W60 on the NUC using a pre-fix read/allocator baseline, compare the
   fixed client on the same cluster/budget, and add a reusable verified-content
   live harness. Update the fence-read harness for the new allocator API.
6. Add D25 immutable per-stream queue admission: canonical digest matching,
   identical retries, sequence high-water and cross-stream rejection.
7. Add exact consumed-terminal tracking. Unknown/RETIRED evidence does not
   authorize consumption; conflicting terminal results retain ownership.
8. Add oldest-consumed-first ACK selection/completion and 64-entry backpressure.
   Test twenty full ring cycles, changed intents and lost/error ACK ownership.
9. Run the queue against actual KV publication/retirement: a newer committed
   intent cannot retire past an older unsubmitted local intent; a lost ACK
   reply retries the original request and retired sequences cannot publish.
10. Charge client queue allocation to the metadata hard bound; free refuses
    retained receipts. Verify allocator failure, credit consumption, saturation
    of the global 8 MiB metadata budget and complete release after retirement.

## Acceptance

- Full Linux `make test`: PASS ([unit log](unit.log)). Initial private-build
  attempts exposed missing copied docs/README and accidentally copied Mac test
  executables; complete source/doc synchronization and forced Linux test
  rebuild resolved these environment issues. The fence-read test stub was
  updated and committed; no unresolved test failure remains.
- Full NUC POSIX: **216 pass, 0 fail, 1 skip** in 76.1 s. The skip is unsupported
  mmap ([log](posix.log)).
- NUC W36 `peer_rename_vs_unlink_src`: **20/20 pass** ([log](w36-20.log)).
- W60 baseline: **2000/2000 tiny reads fail**, 2000 read-NOMEM log lines,
  sequential reader error-free ([result](baseline.log)).
- W60 fixed, identical fixture/budget: **0/2000 failures**, zero read-NOMEM
  lines, sequential reader error-free ([result](fixed.log)).
- Reusable harness checks both sequential and tiny data: **0/2000 failures**,
  27,414,757,376 sequential bytes, error-free and reader joined normally,
  47.7 s including concurrent test activity ([result](verified.log)).
- Body budget on both A/B mounts: **32 MiB normal + 32 MiB drain**.
- Architecture documentation gate: **5/5 PASS**.

The initial private-client fixture write encountered PROTO against old NUC
`79983128` servers. All four servers were rolled to the same current build
before A/B acceptance. The failed diagnostic mount owned only generated test
bytes; it was detached using force-discard after a refused clean drain. Normal
mounts drained cleanly. No production/user data was discarded.

## Remaining work

D25 ACK ownership remains staged: no public FUSE write/flush activation. Its
caller must admit every stream intent in sequence order before submission and
mark consumed only after cache ownership processes the exact terminal result.
I23 session admission/fencing, abandoned-stream recovery/cleanup, coherent
mtime invalidation, both flush integrations and logical-resize gates remain.
The queue is memory bounded, not a client crash-recovery journal.

W60's NUC controlled gate passes; Xorinox full-root mixed `rg` remains owed.
Prefetch-drop diagnostics and sub-chunk charging are follow-up enhancements.
