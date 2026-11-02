# NUC common03bd affected qualification — 2026-10-12

Exact central frozen source `03bd89fb7de56c2c6af44ab3bd26c54caf4b8274`, branch
`qualification-v030-tcp-direct`. Actual change: bounded FIFO Raft inbox slices
with progress publication. Archive SHA256
`78a151f6bb06d3094d6adf6fa999b14eb9ca29e4bf0f9d93da7baf64b37ba53c`.
Archive regular-file inventory matched all23,318 manager tracked-file hashes;
commit/fullmanifest verified before normal build and before/after live gates.
Actual executable versions/hashes recorded: `03bd89fb-group3-normal`.
Initial manifest archive_sha256 schema refusal happened before compilation/live
work. Derived checksum added to verification record (same source/files); manager
subsequently added same field centrally. This is staging context, not productFAIL.

TCP/direct I/O/FILES; flow/earlyretire/groupdrain/repair off. Unique clean roots
on/data1, ports24210–24213. No compilers/profilers/other daemons during live
measurements. Existing helper assertions and budgets unchanged. Seven-client
fixture explicitly4G per node, jobs16 per suite; seven concurrent suites.
Prior def and52 failures preserved separately. No candidate fixes or promotion.

## Results

| Gate | Verdict | Seconds |
|---|---|---:|
| Existing direct/TCP `gc_reclamation.py --posix` core/peer/physical | PASS,exit0 | 95.543 |
| Core | 216PASS,0FAIL,0SKIP,0NOTRUN | 20.3 |
| Peer | 67PASS,0FAIL,0SKIP | 11.3 |
| Quiet seven-client mounted fixture, `--posix --posix-jobs 16 --fixture-quota 4G` | **FAIL**,exit1 | 174.267 |
| Peer after seven suites | 67PASS,0FAIL | 21.2 |
| Existing native actual storeGC, after live cleanup | PASS,exit0 | Raw log |

All seven-client TSVs complete; each221cases with one unsupported-mmapSKIP:

| Client | PASS | FAIL | SKIP | Seconds |
|---|---:|---:|---:|---:|
| 0 | 219 | 1 | 1 | 99.1 |
| 1 | 219 | 1 | 1 | 86.5 |
| 2 | 218 | 2 | 1 | 100.7 |
| 3 | 219 | 1 | 1 | 100.8 |
| 4 | 218 | 2 | 1 | 96.9 |
| 5 | 219 | 1 | 1 | 101.0 |
| 6 | 218 | 2 | 1 | 100.4 |

All seven `dir_deep_nesting` failed unchanged30s deadline. Clients2/6 also failed
`virt_stats_readable`30s; client4 also failed `virt_find_query`30s. No ENOSPC
failures. Bounded inbox slicing alone does not eliminate the observed deep
case deadline here. No drive-latency/hardware-ceiling/W97improvement inference.

Core actual physical deletion, retry ledger, reclamation, queue/counter advance,
restart empty inventory/zeroquota and latePUT controls PASS. Seven-client
run likewise completed acknowledged cold hashes, actual fragment
reclamation and metadata queue drain PASS; report has cold_verified=true
and seven failing POSIX client entries. FinalFAIL
correctly comes from case failures. No global physical inventory confound or
client-stop failure in this run. Existing cold correctness check is not standalone
recovery qualification. Native two-root/format/checksum/quota/syscall/idempotence
GC coverage is primitive coverage, not mounted buffered acceptance.

## Deep test implementation and syscall pattern

Post-cleanup source inspection, exact `tests/posix/posix_suite.py`:
`dir_deep_nesting` creates60components d00…d59 using one `os.makedirs` call on
an absolute path. It buffered-writes four bytes `deep` to a leaf, closes, reopens
and reads/checks those bytes, then `shutil.rmtree(d/d00)` and checks absence.
There is no explicitfsync and no larger @budget annotation: minimum case budget
30s. Worker setup `os.makedirs(tdir,exist_ok=True)` and process startup are also
inside the runner's budget. Each suite allows16workers; seven suites compete
on one four-node cluster, so up to112cases may be active (not112deep cases).

Logical minimum body operations:60MKDIR, one fileCREATE+WRITE/CLOSE,
READ/CLOSE, one leafUNLINK,60directory removals, absence check. Recursive Python
makedirs checks ancestors and builds growing absolute paths. The path-component
sum for the60mkdir targets is1+…+60=1830 beyond the case root; this is potential
path-resolution work before caching, **not measured EFS LOOKUP/RPC count**.
Final depth is60below case directory, plus mount/test-root prefix components.
The different100-depth `dir_deep_nesting_beyond_64` case already uses dir_fd
construction explicitly to avoid quadratic setup, preserving full-path leaf
operations. Do not silently replace this failing60-depth workload or extend its
budget to claim storage improvement.

After all EFS fixtures stopped, exact same function was traced on a newly created
ordinary/tmp directory under NUC LinuxPython, excluding imports/worker/setup by
BEGIN/END markers. Reference counts:

| Selected syscall | Count |
|---|---:|
| mkdir | 60 |
| openat | 62 |
| newfstatat | 121 |
| fstat | 123 |
| getdents64 | 120 |
| read | 2 |
| write | 1 |
| close | 122 |
| unlinkat | 60 |
| rmdir | 1 |
| fsync | 0 |

672selected syscalls; rmtree uses directory descriptors, fd validation/scans,
relative unlink/rmdir. unlinkat includes59directory removals plus leafunlink;
finalrmdir removes topdirectory. Duplicate directory descriptors explain more
closes than openat in this selected trace (fcntl notselected). This normalFS
reference is a syscall-pattern check, **not live FUSE/RPC profiling or EFS timing**.
Which phase consumes the30s and how many actual RPCs/ancestor validations occur
remain unmeasured here; targeted phase/RPC isolation is the manager's next task.
Raw marked trace, reference script, counts and exit retained with gate evidence.

## Evidence and final state

`results/measure/20261012-nuc-03bd-group3/controller/` retains full inputmanifest,
build/images, command/env/source/exit/timing, logs and post-cleanup reference.
`fixtures/` retains complete TSVs, manifests and physical reports.
Collected archives retained. Remote `/home/efs/nuc-03bd-group3`, isolated stores
`/data1/efs/evidence/nuc-03bd-group3`.
All owned daemons and mounts stopped; no EFS cluster currently running. No
old-binary/data retention/backwards qualification or unrelated host-service
changes. Manager received exact failures and implementation analysis. Prior
W52/fullbothmodeW112 and W97sustainedcomparison are not closed by these narrowed
gates; RDMA/repair remain deferred. No pushes/tags/promotions.

**Raw evidence note (2026-10-23):** the bulk data in `nuc-03bd-group3` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
