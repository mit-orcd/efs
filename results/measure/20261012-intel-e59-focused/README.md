# Intel focused READDIR candidate qualification

Frozen source `e59e815fab2916340baad585dc08712ed182b7ea`, central archive SHA-256 `3ae2aeb2a9befd7ce8e2639a74c1bc23594ee5dacac249267f2e8f0f9b222eae`; 23,318 inputs verified before and after. Build PASS before quiet run; no profiling or competing build. Fresh private four-daemon local Intel1 files/TCP/direct fixture with 4G per daemon, seven clients, jobs16, unchanged 30-second case deadlines. Actual four daemon and seven client environments verified EFS_COMMANDS=0, EFS_COMMANDS_SERVER=0, EFS_PACKED_WRITES=1, EFS_TRANSPORT=tcp.

**FAIL**: 71/84 cases passed, 13 failed, no skip/notrun. All ten original ordinary I/O cases passed on all seven clients (70/70), preserving previous focused result. `dir_readdir_listing` timed out at30s on all seven clients. `names_crazy_dirs` timed out at30s on clients0–5; client6 passed. The extra scope means this is not a matched comparison against previous ecdc ten-case-only PASS and does not establish a new regression caused by READDIR reuse.

All seven graceful client stops passed. Controller finished idle with no remaining private processes/mounts. Daemon retirement is cleanup, not a separate graceful daemon acceptance gate. All48 raw evidence files hash/size verified. Source/binary hashes and original runner commands preserved. External selected-registry harness preserves original case functions/assertions; reduced scope does not replace AMD broad qualification.

READDIR optimization remains logically reviewed but has not closed directory concurrency timeouts. No implementation edit, deadline change, repeat/profiler launch, production deployment, push, tag or promotion. Coordinating root was informed immediately of directory failures.
