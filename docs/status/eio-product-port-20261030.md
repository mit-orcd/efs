# ORCD EIO stress repair — product port to devel

**Verdict:** PASS focused. 6 repairs ported (K66–K71), 9 commits skipped. Branch `claude/eio-port-c3d1de`
on devel `7514b2145`; source `agent/orcd-eio-stress-repair-20261028` (15 commits, ~294 behind devel).
Not run: any live cluster or IOR gate (deferred; the branch's own live record is
[the repair page](orcd-test-eio-stress-repair-20261028.md), which stays FAIL overall).

**Method:** each branch test was adapted to devel's current test text and run on devel source without
the fix (fail), then with it (pass). Tests that did not compile on devel were re-based on devel's copy and
only the branch's delta applied. Host: macOS for the Python extraction tests; intel1 (Linux x86_64) for
`make all` (clean) and `make test-report-pressure test-fold-observation` (pass), tree exported without `.git`.

| Branch commit | Disposition | Reason / devel reproduction |
|---|---|---|
| `e7f9a89a1` retain failed POSIX fixtures, report-size-missing diagnostics | **Skipped** | Test tooling and `fprintf` diagnostics, no product behaviour; the retention test also fails against devel's suite for unrelated harness reasons. |
| `d8a7df06c` mix allocation bits into dcache buckets and locks | **Skipped** | Already on devel (`5a820f91c`); `test_dcache_lock_distribution` passes unmodified. |
| `de0bb5dc7`, `0958c03dd`, `36e566175` pin staged inode across OPEN | **Ported** (K69) | `test_attribute_authority_errno` eviction case asserts `row_present` on devel: fails; passes with `efs_fuse_open_ino` pin wrapper. |
| `1b840457a` existing-file CREATE through OPEN handoff | **Ported** (K70) | `test_create_credentials` fallback case fails (`fallback_opens==1`) on devel. |
| `4837a054e`, `0bb93e9dd`, `b05ff3483`, `afb0e1ee3` | Docs only | Evidence merged into the repair page; see coordinator note there. |
| `897a79b64` read-cache key distribution | **Ported** (K68) | `test_rdcache_steal` 64 allocator-stride files: `live==64` fails on devel. |
| `585411524` bound POSIX mismatch diagnostics | **Skipped** | Test-suite diagnostics only. |
| `d30594aa6` reuse truncate quorum fences | **Ported** (K67) | `test_truncate_read_reuse` expects 1 read-index per covered group; fails on devel. |
| `6cad51b02` bound lease cleanup admission | **Ported** (K71) | `test_open_retry_fairness`, `test_open_retry_lock_budget` (EFS_ERR_AGAIN) and `test_opid_admission` (inflight table = receipt window) all fail on devel. |
| `098333947` reply to proved append reservations without collect | **Ported** (K66) | `test_append_reservation_reply`: `transactions==1 && !collects` fails on devel. |

**Not ported, by design:** the `flush-prepare-fail`, `dcache-flush-fail` and `report-size-missing`
diagnostic prints (no failing reproduction possible). The cumulative branch diff was applied and the
diagnostic hunks and the already-present dcache hunks reverted.

**Reproduce:** `python3 tests/test_{append_reservation_reply,truncate_read_reuse,attribute_authority_errno,create_credentials,rdcache_steal,open_retry_fairness,open_retry_lock_budget,opid_admission}.py`
(each aborts on devel `7514b2145` source, exits 0 on this branch). New tests are wired into
`test-report-pressure` and `test-fold-observation` (`python3 tests/test_make_test_recipe.py` PASS).

**Limitations:** all tests extract the real function text into a stubbed harness; none exercise a live
cluster. The branch's own live qualification (272 FAIL / 372 NOTRUN stress, truncate ambiguity) is not
re-run; K66, K69 and K70 block alpha pending a live gate.
