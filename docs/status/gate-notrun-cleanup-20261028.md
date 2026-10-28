# Task W — make supported gate-host coverage run

Branch `agent/devel-task-w`, base devel `3debca7ec`: IN PROGRESS.
Untouched-base `make test-all` is running on intel2; before/after gate lines,
exact tested commit, raw hashes and cleanup remain pending. Product settings
and runtime behavior are unchanged. No package installation is authorized or
performed. Existing clusters are untouched.

## Changes and scope

- Provision the cluster-stop/remount test's six required source scripts from
  cluster repository commit `301e21671afec9025e8a1f08735e49bf6cac2a0c`. The Mac
  repository is read-only; its unrelated local testing edits are not exported.
  Only committed named files are exported, with hashes/provenance; no deployment
  configuration or credentials are copied. Tests use mocked transports.
- Build a dedicated fault-linked follower-bound binary and run its real parked
  compactor subcase explicitly in `make test-long`. The normal binary no longer
  advertises missing fault coverage; its wrong-binary argument fails explicitly.
- Repair the newly unwired graph-validator entry: `make code-graph-check` is a
  named developer gate requiring a generated graph and Git index, not a pretend
  hardware skip in an archived runtime-gate tree.
- Document exact RPM prerequisites and distinguish synthetic refusal self-tests
  from actual unavailable detectors. Existing leak/sanitizer self-test corrections
  on devel are retained; genuine runtime failures stay visible.

## Read-only package inventory (October 28)

All four hosts report Rocky Linux 10.2 x86_64, GCC/runtime packages
`14.3.1-4.4.el10`, Valgrind `3.26.0-5.el10`, FUSE `3.16.2-5.el10`, and
`ptrace_scope=0`. GCC, gcc-c++, glibc-devel, libasan, libubsan and libtsan are
installed on all four. Package availability is not detector qualification.

| Host | Valgrind executable | `valgrind-devel` / Memcheck headers | ASan/UBSan/TSan runtime packages |
| --- | --- | --- | --- |
| intel1 | installed | **missing** | installed |
| intel2 | installed | **missing** | installed |
| nuc | installed | **missing** | installed |
| xorinox | installed | installed | installed |

Task W makes no package/kernel/ptrace change. The user can decide whether to
provision the three missing header packages. RDMA device paths remain separate
hardware prerequisites and will not be turned off or counted as passing.
