# Recorder shutdown ownership fix

October 11, 2026. Focused S6 correctness checkpoint on `devel-code-review`; [release disposition](../reviews/release-disposition-20261011.md). No profiling run or live process was started/stopped by the regression.

## Problem and fix

Perf/strace stop implementations broke out of their polling loop after successful `waitpid`, then still called SIGKILL and waited again on the now-reaped numeric PID. The process no longer owned that PID. The bug existed in both benchmark launchers, daemon/FUSE perf/strace wrappers, and the embedded FUSE benchmark stop path.

A small internal stop helper now checks `waitpid(WNOHANG)` before the first signal. A successful reap or ECHILD clears the handle and returns immediately. Otherwise it sends TERM once, polls for up to 50 attempts with the existing 100 ms delay, and uses KILL followed by reaping only if the child remains owned at timeout. Poll EINTR retries within the bounded attempt count; blocking reap retries EINTR. Unknown wait errors retain the handle without signaling; a completely interrupted ownership check also sends no signal. Repeated stop on a cleared handle is inert.

This helper assumes the caller owns the child and is its only reaper, as the original recorder lifecycle does. It is not a pidfd implementation and does not claim to solve arbitrary concurrent reaping by other threads. No public API, recorder start/exec arguments, target selection, output path, perf frequency or strace filter changes. Sharing only the stop helper avoids a broader recorder-launch framework refactor.

## Before/after regression

`tests/test_recorder_stop.py` extracts each actual stop wrapper and includes the shared helper, replacing kill/waitpid/usleep with recording stubs. It also verifies the embedded FUSE benchmark uses that helper. It covers: already-exited child, normal TERM/reap, ECHILD before/after TERM, EINTR followed by successful ownership check, timeout KILL with interrupted blocking reap, all-interrupted polling, unexpected wait error, and repeat/zero-handle stop.

Before modification, `python3 tests/test_recorder_stop.py --revision 1e7d87f4` fails the no-signal-after-reap assertion (`pid == 123 && !reaped` in the kill stub). Current `python3 tests/test_recorder_stop.py` passes all cases across the four actual wrappers and shared helper. The test sends no real signal and sleeps zero wall time. It is wired into `make test` and `make test-client-memory`.

Client benchmark and daemon full-file syntax checks pass locally. The local benchmark syntax check passes with `-DO_DIRECT=0` as a macOS-only compile shim for its existing Linux direct-I/O references; this does not validate direct I/O. FUSE validation here is the extracted actual wrappers/call-site check, not a full Linux FUSE build. Cache-memory/read-pressure and rollup-budget sanitizer regressions remain passing. Documentation/whitespace gates pass.

## Branch compatibility and release acceptance

All four original stop bodies are byte-identical in inspected ORCD `8d07b45adac9c6aa24ba46f7e8e1d4cc163f4da5`, NUC `3538213cfe793dbdb9025c3e0f65cc96eb97149a`, and MAC `b2c01fa05afa1287cd358f352ebfbc831b570e31` relative to the review baseline. The embedded FUSE benchmark uses the same helper without changing report generation. No other worktree/branch was edited.

Coordinator still owns full merged Linux builds and real recorder exec failure/early exit/normal shutdown/timeout acceptance, with profiling overhead reported separately. Query compatibility gates remain required. This checkpoint resolves the reproduced stop-after-reap defect; it is not mounted filesystem validation.
