# efs_fuse.c split (Task CW, W171 phase 3)

Task CW, Nov 4 2026. Purpose: [W171 phase 3](structure-plan-20261102.md): split `src/client/efs_fuse.c` (7,956 lines) into `src/client/efs_fuse/` by the 2b recipe so an agent changing one FUSE area reads `src/client/README.md`, `fuse_internal.h` and one file instead of the whole file. Verdict: **PASS** (fstor007 gate `FAIL 0`; no K row; blocks alpha: no).

## Plan (target files, in the original order)

The split is contiguous: each target file is one run of the original file, in original order, so `tests/csrc.py` concatenates `fuse_internal.h` and the files back to the original text (apart from linkage). That keeps every test that slices the source from function A to function B working.

| File | Original lines | Content |
|---|---|---|
| `support.c` | 53-1329 | crash handlers, stats, features, file handles, virtual inodes and query results, invalidation thread, find, access checks |
| `getattr.c` | 1330-1875 | lookup memo, getattr, readdir collection |
| `open.c` | 1876-2775 | open references, lease acquire and retry, open, read |
| `writeback.c` | 2776-3315 | write-back pool, bounce buffers |
| `append.c` | 3316-3842 | append reservation and publication, pressure flush |
| `write.c` | 3843-4477 | write admission, store, buf, fsync, flush |
| `namespace.c` | 4478-5119 | create, mkdir, unlink, statfs, chmod/chown, symlink, link, utimens, truncate |
| `release.c` | 5120-5427 minus destroy | release, control drain, rename, lseek |
| `plock_init.c` | 5428-5803 | POSIX locks, flock, `FUSE_INIT` |
| `lookup.c` | 5804-6134 | lookup, setattr, deep-callback sampling |
| `ll_run.c` | 6135-7006 | the `ll_*_run` bodies, read, readdir and friends |
| `main.c` | 5219-5315 and 7007-7956 | destroy, mutation-gated `ll_*` wrappers, operation table, startup, `main` |

`fuse_internal.h` holds the include block, the types and macros used by more than one file, and a declaration for every function and variable that was `static` and is now used in another file.

Two header-only modules carry static state and must stay in one translation unit: `stop_control.h` (`g_stop`: used by destroy, the mutation wrappers and `main`) and `reply_buffers.h` (a thread-local key: used by read and readdir only). That is why `efs_fuse_destroy` and its `g_unmount_unclean` state move to `main.c` (next to the wrappers and `main`), and why neither header is in `fuse_internal.h`.

## What moved

`src/client/efs_fuse.c` (7,956 lines) is gone. Line counts after: `support.c` 1,249, `getattr.c` 527, `open.c` 876, `writeback.c` 498, `append.c` 529, `write.c` 637, `namespace.c` 644, `release.c` 210, `plock_init.c` 377, `lookup.c` 329, `ll_run.c` 875, `main.c` 1,051, `fuse_internal.h` 465 (8,267 in all; the largest file is 1,249 lines, and an agent changing one area reads the header plus one file, about 1,700 lines at most).

Changes besides the move, all in `fuse_internal.h` and the commit that exposes each symbol: `static` removed from 172 functions and variables used across files (each declared in the header); the types and macros used by more than one file moved to the header; `g_wb` and `g_find_res` were anonymous structs and gained the tags `efs_wb_state` and `efs_find_res` (the only text change, needed to declare them `extern`); the include block's relative paths follow the new directory; `commands/source.h` (static data), `reply_buffers.h` and `stop_control.h` (static state) are included only by the files that use them. Check: normalized line-multiset of the new files against the old file differs only by those lines, the include paths and the declarations.

`efs_fuse_destroy` and its unclean-unmount state moved from the release area to `main.c`: `stop_control.h` keeps `g_stop` as a `static` in each including file, and destroy, the mutation-gated wrappers and `main` all use it, so they must share one translation unit. This is a reorder of top-level definitions, not of anything inside a function.

## Build, csrc and docs

`Makefile`: `CLIENT_SRCS` lists the twelve files; the three per-object dependency rules name `$(CLIENT_OBJS)`. `tests/csrc.py`: `MODULES['efs_fuse']` is the twelve files in original order followed by `fuse_internal.h` (header last, so a source search finds a definition before its declaration); new helpers `hblock()` (a type from the header) and `upto()`. 56 tests read the source: 52 converted by script to `csrc.module('efs_fuse')`, 4 by hand; their search patterns are static-agnostic; 7 tests that sliced a moved type take it from the header with `hblock()`. Assertions and compiled code are unchanged. `src/client/README.md`, `developing.md`, `settings.md` (source column), one dangling link in the tiny-file plan, the `ah_measure.py` shim loop, `architecture-full.md` and the code graph were updated. Checks: `make docs-check`, `check_doc_links.py`, `gen_settings.py --check`, `env_inventory.py --check`, `test_make_test_recipe.py`, `csrc.py`, graph rebuild and `test_code_graph.py` pass. Locally (macOS, no build): the 56 readers pass except `test_faults_env_ignored`, `test_put_drain`, `test_namespace_bounds`, which need `libefs.a` (same before and after).

## Gate

Source: branch `claude/magical-feistel-8ef774` at `d5d02b38f` (base devel `15f346b48`), `git archive` to `/tmp/efs-taskCW` on fstor007 (96 cpus, Linux, private libfuse 3.16.2 via a shim, TCP, buffered I/O), `make clean`, `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh`. Result: **TOTAL 584 FAIL 0 NOTRUN 3 STATUS INCOMPLETE (710 s) OVERBUDGET 2** (NOTRUN: cluster stop scripts absent, no memcheck header, RDMA hardware; OVERBUDGET `test_k38_shared_chunk_pressure` 89 s vs 70 and `test_pump_logging` 109 s vs 95). Build: 9 warnings, none in `efs_fuse/`, all present before. No FAIL, so no solo reruns. The build and link are unchanged otherwise (no duplicate symbols).

F2 TSan lane (`make tsan-check`, 22 entries): one failure, `d4_progress`: a data race in `efs_dcache_maybe_reclaim` (`src/client/write.c:9009`), not in split code. On unsplit devel `15f346b48` the same entry gave 1 FAIL (same race, same line) and 2 PASS in 3 runs; the split tree gave 3 of 3 PASS on rerun. The race is intermittent and existed before the split; it is an existing finding for the coordinator (not opened as a K row here: no difference caused by the split). All other TSan entries PASS.

## Reproduction

```
git archive HEAD | tar -x -C /tmp/efs-taskCW    # then, with the private libfuse on PKG_CONFIG_PATH and LD_LIBRARY_PATH
cd /tmp/efs-taskCW && make clean && EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh
make tsan-check          # or EFS_SAN_ONLY=d4_progress make tsan-check, repeated
python3 tests/csrc.py && for t in $(grep -lE "csrc.module\('efs_fuse'" tests/test_*.py); do python3 $t; done
```

Raw output is outside Git: `~/efs-artifacts/taskCW-20261104/` on fstor007 (`summary.txt`, `timings.tsv`, `build.log`, `progress.txt`, `gate.log`, TSan logs for the first run and the 3+3 reruns, one TSan report), `SHA256SUMS` sha256 `3823aa9ec42e883e70772403f1596330ff384a8b5ab4ff120bfa8df4fc6f3872`. The scratch trees were removed.

## Limitations

macOS cannot build `efs-fuse`, so intermediate commits were checked by syntax (no new diagnostics against the unsplit file) and the 53 runnable readers, and only the final tree was built and gated. The other phase-3 splits and branches that edit `efs_fuse.c` (`agent/devel-task-ao`, `agent/orcd-eio-stress-repair-20261028`, `agent/task-h-repair`, `devel-w123-recovery`) conflict with this move and must be reapplied by the coordinator.
