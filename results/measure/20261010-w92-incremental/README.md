# W92 incremental-cache evidence, October 10, 2026

Branch devel-find-stats; exclusive Xorinox VMs; files/direct-I/O/TCP. No pushes,
wipes, container changes, D25 or authoritative checkpoint policy changes.

Code boundaries: baseline builder 0027c686 (after three merges), round 1
10febf8b, COW guard f3386fe5, round 2 ba89a878, round 3 d83f3f06.
branch-boundary.txt captures the 15 local branch tips; branch-coverage.tsv proves
all captured tips are ancestors. Later independent branch changes are outside
this measurement boundary.

summary.json and cold-summary.json are derived by summarize.py and the cold
oracle tally. Native/full/live/cold logs are separate. Original failures and
passing retries are retained: initial authentication, fixture admission assertions,
fresh-build fallback initialization, and generated-document mismatch. All fixes
precede the deployment whose results are reported. Initial runtime source hashes
are named runtime-before-source-sync; final-runtime logs verify synchronized
production source. Code binaries were built privately and installed only after
client/daemon drain.

Exact builder sources are baseline-query-disk.c and variant1/2/3. The controlled
fixture is /home/efs/w92-frozen-after (25425 files, 827 dirs, 6234928 bytes).
final_frozen.sh compiles identical supporting libraries, runs strace separately,
and performs twelve alternating untraced trials via repeat_frozen.py. Daemon
refresh scheduling is outside this builder fixture. Whole-process strace includes
KV work and one fresh build, so its IO totals are not per-refresh cache counters.

profile_driver.sh captures all three daemons for 60 seconds at 99 Hz and creates
6000 random 1–512 byte files with four workers. Compiler/timing work does not
overlap captures. Raw perf data remains in /home/admin/w92-incremental-three-rounds
on each node; profile-integrity logs record all nine successful exits and SHA256s.
Text reports record zero lost samples. Do not sum inclusive nested percentages.

Deployment: prepare_key_wrapper.sh uses the existing cluster key for legacy
helpers; deploy_driver.sh handles drain/install/start. No credentials change.
The private build is /home/efs/w92-incremental-build on xefs1. final_checks.sh
runs the complete native suite, core/two-client/extras, prepare/restart/verify and
all retained tiny-tree oracles. restore_driver.sh removes the diagnostic mount,
checks ordinary .efs opt-out and copies source to all six VMs. Final integrity
records the deployed code build ID and current source hashes.

Drivers preserve old namespace trees for cold oracles. Re-running a profile with
the same directory or model names is not a new equivalent trial. Cold restart
checks are not hardware power-loss testing. Final status and open acceptance:
../../../docs/status/query-cache-incremental-20261010.md (repository-relative
from the results directory: use the status index if browsing from a renderer).
