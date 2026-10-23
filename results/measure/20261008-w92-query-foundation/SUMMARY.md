# SUMMARY — results/measure/20261008-w92-query-foundation

W92 query foundation tests: snapshots/dirty hints + name indexes, NUC and Mac
(incl. sanitizers).

## Purpose and linked item

- **W92** foundation: source commits `3ecfec32` (snapshots/dirty hints),
  `5d7e1175` (name indexes).

## Verdict

**PASS**: full test_kv_lsm and test_name_index pass on the NUC build; Mac Clang
focused snapshot + name-index suites pass under ASan/UBSan (LeakSanitizer
unsupported on this Mac → detect_leaks=0). kv_snap.c compiler warnings are
pre-existing Linux qsort_r vs Mac/BSD ABI, kept visible.

## Source commit and build identity

Final build logs include `EFS_GIT_ID=5d7e1175`; this does not change the live
cluster `e9f974b4e148`.

## Environment

NUC `/data1/efs/evidence/w92-build` (not the deployed cluster source); Mac Clang +
ASan/UBSan.

## Method

Full kv_lsm/name-index unit tests (NUC); focused snapshot + name-index suites
with sanitizers (Mac).

## Results

All listed suites pass; pre-existing kv_snap.c qsort_r ABI warnings noted.

## Limitations and remaining gates

LeakSanitizer unavailable on Mac; the live cluster build is unchanged by this
evidence.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  test logs in this dir

**Raw evidence note (2026-10-23):** the bulk data in `w92-build` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
