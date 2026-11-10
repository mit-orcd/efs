# K1 / O4: the devops `stop.sh` patch applied on a branch (Task FJ, phase 1)

**Status (Nov 10 2026): applied on devops branch `k01b-stop-nonorderly`, commit `f2a1fc5d91f84748220da91e43394e0eda73726d`, on top of devops `devel` `8bc872e`. Not merged into devops `devel`. Not deployed to any host. O4 is still NOTRUN, K1 stays VERIFY. Phase 2 (merge, push, run O4) waits for the user's approval.**

Linked item: Alpha 1.0 Stage 1 row K1 (W145), run O4 of the [acceptance matrix](../stage1-acceptance-matrix-20261030.md). The user lifted the standing rule against editing `~/git/devops` for this task only, limited to `nuc-efs/stop.sh` and its fixture `nuc-efs/test_daemon_ownership.py`. Work was done in the separate devops worktree `/Users/mike/git/devops-k01b`. The devops main checkout (`~/git/devops`) stayed on `devel` with `git status --short` empty; `~/git/cluster` was only read.

## What the patch must achieve, and why

[R4](../2026-10/k01b-r4-shutdown-deadline-20261022.md) made `efsd` bound its whole shutdown and made `scripts/server_processes.py stop-file` report the outcome from the daemon's exit record: rc 0 orderly, rc 3 forced (stores preserved), rc 4 barrier failed or outcome unknown (no record from this PID). [T2](../2026-10/k01-t2-forced-exit-bound-20261022.md) bounded the forced exit itself. `nuc-efs/stop.sh` runs under `set -e`, so with those codes it aborted at the first non-orderly daemon: later daemons kept running (the original W145 symptom, "stop.sh then leaves the remaining daemons running"), and no line said which node was non-orderly. The patch must therefore (1) stop every configured local daemon, (2) name every non-orderly one on stderr, (3) exit non-zero with the `stop-file` code, so an operator or `scripts/qualify.py` never reads a forced or unknown stop as an orderly restart, and (4) lose nothing else the script does. O4 needs this to observe a forced stop of one orcd-mini daemon with the others still stopped.

The patch is the 57-line file `k01b-devops-nuc-efs-stop-r4.patch` from the coordinator's notes folder (SHA-256 `cbf3879df9d2e9992cd715ec25d3f95e863b21fa484be59c7314c82dc3ea993d`). Its `stop.sh` hunk applied unchanged. Its test hunk did not apply: today's `test_daemon_ownership.py` already has a fake `efsd` that writes the orderly exit record (it was fixed separately), so only the new assertions and the non-orderly case were missing. Those were re-made by hand.

## The change (`git diff devel k01b-stop-nonorderly` in the devops repo)

```diff
diff --git a/nuc-efs/stop.sh b/nuc-efs/stop.sh
index 9cfd396..f0499a7 100755
--- a/nuc-efs/stop.sh
+++ b/nuc-efs/stop.sh
@@ -87,10 +87,24 @@ for pidfile in "$LOGS"/efsd-n*.pid; do
     fi
 done
 # Private fixtures may use the same node IDs on different roots and ports.
+# Every configured daemon is stopped even when one stops non-orderly
+# (server_processes.py: 0 orderly, 3 forced with stores preserved, 4
+# barrier failed or outcome unknown); only all-orderly is a clean stop.
+stop_rc=0
+nonorderly=""
 for i in "${!PORTS[@]}"; do
     [ "${NODE_HOSTS[$i]:-$LOCAL_HOST}" = "$LOCAL_HOST" ] || continue
     id=$((i + 1))
+    rc=0
     python3 "$BASE/src/scripts/server_processes.py" stop-file \
-        "$LOGS/efsd-n$id.pid" "${STORDIRS[$i]}"
+        "$LOGS/efsd-n$id.pid" "${STORDIRS[$i]}" || rc=$?
+    if [ "$rc" -ne 0 ]; then
+        nonorderly="$nonorderly n$id(rc $rc)"
+        [ "$stop_rc" -eq 3 ] || stop_rc=$rc
+    fi
 done
-echo "$EFS_CLUSTER-efs stopped"
+if [ "$stop_rc" -ne 0 ]; then
+    echo "$EFS_CLUSTER-efs stopped, NOT orderly:$nonorderly; stores preserved" >&2
+    exit "$stop_rc"
+fi
+echo "$EFS_CLUSTER-efs stopped (orderly)"
diff --git a/nuc-efs/test_daemon_ownership.py b/nuc-efs/test_daemon_ownership.py
index 82de5be..449d212 100644
--- a/nuc-efs/test_daemon_ownership.py
+++ b/nuc-efs/test_daemon_ownership.py
@@ -32,21 +32,22 @@ with tempfile.TemporaryDirectory(prefix='efs-deploy-ownership-') as tmp:
         f'STORDIRS=({storage[0]} {storage[1]})\nMOUNTS=({base}/mnt)\n')
     source = base / 'daemon.c'
     # A stand-in efsd: STORAGE is its last argument (EFS K33 CLI), and on
-    # SIGTERM it writes the orderly exit record the stop path requires.
-    source.write_text('#include <signal.h>\n#include <stdio.h>\n#include <sys/stat.h>\n#include <unistd.h>\n'
+    # SIGTERM it writes the orderly exit record the stop path requires, unless
+    # FAKE_UNORDERLY is set (no record: server_processes.py reports rc 4).
+    source.write_text('#include <signal.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <sys/stat.h>\n#include <unistd.h>\n'
                       'static volatile sig_atomic_t stop;\n'
                       'static void done(int s){(void)s;stop=1;}\n'
                       'int main(int argc,char **argv){signal(SIGTERM,done);while(!stop)pause();\n'
                       ' char d[4096],f[4200];mkdir(argv[argc-1],0755);snprintf(d,sizeof d,"%s/log",argv[argc-1]);mkdir(d,0755);\n'
-                      ' snprintf(f,sizeof f,"%s/efsd.exit",d);FILE *o=fopen(f,"w");\n'
+                      ' snprintf(f,sizeof f,"%s/efsd.exit",d);FILE *o=getenv("FAKE_UNORDERLY")?NULL:fopen(f,"w");\n'
                       ' if(o){fprintf(o,"efsd-exit 1\\npid=%d\\noutcome=orderly\\nrc=0\\nput_sync=done\\n"\n'
                       '  "reclaim_journal=done\\nusage_flush=done\\n",(int)getpid());fclose(o);}\n'
                       ' return 0;}\n')
     binary = base / 'efsd'
     subprocess.run(['gcc', str(source), '-o', str(binary)], check=True)
 
-    def spawn(path, *arguments):
-        process = subprocess.Popen([str(path), *arguments])
+    def spawn(path, *arguments, env=None):
+        process = subprocess.Popen([str(path), *arguments], env=env)
         children.append(process)
         return process
 
@@ -75,7 +76,8 @@ with tempfile.TemporaryDirectory(prefix='efs-deploy-ownership-') as tmp:
             file = pidfile(number, process)
             subprocess.run([sys.executable, str(helper), 'record', str(file), str(store)], check=True)
         owned = children[-2:]
-        stop()
+        result = stop()
+        assert result.stdout.strip().endswith('intel-efs stopped (orderly)'), result.stdout
         for process in owned:
             assert process.wait(timeout=2) == 0, 'owned daemon did not stop gracefully'
         assert unrelated.poll() is None and private.poll() is None
@@ -86,6 +88,21 @@ with tempfile.TemporaryDirectory(prefix='efs-deploy-ownership-') as tmp:
         assert file.exists() and private.poll() is None, 'unknown configuration was guessed'
         assert retained.poll() is None, 'known daemon was stopped before rejecting unknown configuration'
         file.unlink()
+        # A non-orderly stop of n1 (no exit record: outcome unknown, rc 4) must
+        # not leave n2 running: every daemon is stopped, the failure is named
+        # on stderr, the script exits with n1's code and prints no success line.
+        retained.terminate()
+        retained.wait(timeout=2)
+        unrecorded = spawn(binary, '127.0.0.1:17432', str(storage[0]), env={**os.environ, 'FAKE_UNORDERLY': '1'})
+        orderly = spawn(binary, '127.0.0.1:17433', str(storage[1]))
+        for number, process in ((1, unrecorded), (2, orderly)):
+            file = pidfile(number, process)
+            subprocess.run([sys.executable, str(helper), 'record', str(file), str(storage[number - 1])], check=True)
+        result = stop(expected=4)
+        assert orderly.wait(timeout=2) == 0, 'daemon after the non-orderly one was not stopped'
+        assert unrecorded.wait(timeout=2) == 0
+        assert 'NOT orderly: n1(rc 4); stores preserved' in result.stderr, result.stderr
+        assert 'n2' not in result.stderr and 'stopped (orderly)' not in result.stdout, (result.stdout, result.stderr)
     finally:
         for process in children:
             if process.poll() is None:
@@ -95,4 +112,4 @@ with tempfile.TemporaryDirectory(prefix='efs-deploy-ownership-') as tmp:
             except subprocess.TimeoutExpired:
                 process.kill()
                 process.wait()
-print('Deployment ownership: unrelated/stale/private PIDs retained, owned daemons gracefully stopped, unknown records refused PASS')
+print('Deployment ownership: unrelated/stale/private PIDs retained, owned daemons gracefully stopped (orderly line), unknown records refused, a non-orderly stop still stops every daemon and exits with its code PASS')
```

Only the stop loop and the final message of `stop.sh` changed; `git diff` shows no other line of the script touched. The test changed in four places: `#include <stdlib.h>` and `FAKE_UNORDERLY` (the fake `efsd` then writes no exit record, so `stop-file` reports outcome unknown, rc 4), an `env` argument on `spawn`, the orderly-line assertion on the existing owned-daemon stop, and a new non-orderly case at the end. Every existing assertion is kept.

## Behaviour table of `nuc-efs/stop.sh`

"Before" is devops `devel` `8bc872e`; "after" is `f2a1fc5`.

| # | Input or step | Before | After |
|---|---|---|---|
| 1 | Inputs: `EFS_CLUSTER` (default `nuc`), `$1` one of none, `--list-mounts`, `--daemons-only`, `--clients-only`; `env.sh` sources `clusters/$EFS_CLUSTER.env` (`BASE`, `HOST`, `LOGS`, `PORTS`, `NODE_HOSTS`, `STORDIRS`, `MOUNTS` or `MNT`/`MNT2`, `SINGLE_MOUNT`, `EFS_USER`, `EFS_LOCAL_HOST`) | as listed | unchanged |
| 2 | `env.sh`: no `clusters/$EFS_CLUSTER.env` prints `no cluster config ...` (stderr), exit 1; with `$BASE/src` present the user must be `EFS_USER` and not root, else exit 1 | as listed | unchanged |
| 3 | `env.sh`, driver without `$BASE/src`, `--list-mounts`: `ssh` `findmnt` per host, prints `host: target`, exit 1 if an ssh failed | as listed | unchanged |
| 4 | `env.sh`, multi-host driver (`HOSTS` > 1, no `$BASE/src`): `--clients-only` on each host (`\|\| exit 1`), then `--daemons-only` on each host (`\|\| rc=1`), `exit $rc` | as listed | unchanged code. A host whose daemons stop non-orderly now fails it (before: also failed, at its first non-orderly node), so the driver still exits 1 |
| 5 | `stop.sh` lines 9 to 11: no `$BASE/src` (run from the Mac) re-execs itself over `ssh "$HOST"`; the exit status is the remote script's | as listed | unchanged; the remote status may now be 3 or 4 |
| 6 | `--list-mounts` on the host: print the configured mounts that are FUSE-mounted, exit 0 | as listed | unchanged |
| 7 | `intel`: unless `--daemons-only`, `client.sh stop` each present configured mount; `ERROR: mount retained: T` (stderr), exit 1; with `--clients-only`, exit 0 here | as listed | unchanged |
| 8 | other clusters: print `unmounting T`, `client.sh stop T \|\| true`; a mount still present gets `WARNING: clean unmount of T refused ...` (stderr) and `client.sh stop --force-discard T \|\| true`; still mounted: `ERROR: still mounted: ...`, exit 1 (this branch ignores the two flags, as before) | as listed | unchanged |
| 9 | Ownership pre-check: any `$LOGS/efsd-n*.pid` that is not a configured local node prints `ERROR: unconfigured daemon record retained: ...; inspect its ownership before cleanup`, exit 1, before any daemon is signalled | as listed | unchanged |
| 10 | Stop loop: for each configured local node in order, `server_processes.py stop-file $LOGS/efsd-nN.pid <store>`; its own lines go to stdout (`Stopping owned efsd PID ...`, `efsd PID N stopped, orderly` or `forced` or `not an orderly stop`; `ERROR: ...` on stderr) | first non-zero `stop-file` exit aborts the script (`set -e`): later daemons are not signalled | **changed:** the loop goes on; each non-zero node is recorded as `nN(rc R)` |
| 11 | Exit code | 0 when every node returned 0 (also for an absent pid file or a retained stale identity, which `stop-file` reports as 0); otherwise the first non-zero `stop-file` code (1, 3 or 4) | **changed:** 0 when every node returned 0; otherwise 3 if any node returned 3, else the latest non-zero code (see the review below) |
| 12 | Last line | stdout `<cluster>-efs stopped` | **changed:** all zero: stdout `<cluster>-efs stopped (orderly)`. Any non-zero: no stdout line, stderr `<cluster>-efs stopped, NOT orderly: nA(rc R) nB(rc R); stores preserved` |

Stub run that shows rows 10 to 12 (`scripts/evidence/k01/stop_sh_exit_codes.sh`, three nodes, `stop-file` stubbed with scripted codes, runs on macOS):

| `stop-file` codes n1,n2,n3 | Before: exit, nodes asked to stop | After: exit, nodes asked to stop, stderr names |
|---|---|---|
| 0,0,0 | 0, n1 n2 n3, `... stopped` | 0, n1 n2 n3, `... stopped (orderly)` |
| 0,4,0 | 4, n1 n2 | 4, n1 n2 n3, n2(rc 4) |
| 4,0,0 | 4, n1 | 4, n1 n2 n3, n1(rc 4) |
| 3,0,4 | 3, n1 | 3, n1 n2 n3, n1(rc 3) n3(rc 4) |
| 4,0,3 | 4, n1 | 3, n1 n2 n3, n1(rc 4) n3(rc 3) |
| 4,3,4 | 4, n1 | 3, n1 n2 n3, all three |
| 1,0,4 | 1, n1 | 4, n1 n2 n3, n1(rc 1) n3(rc 4) |
| 0,0,3 | 3, n1 n2 n3 | 3, n1 n2 n3, n3(rc 3) |

### Review of the kept exit code

The patch keeps 3 once any node returned 3, otherwise the latest non-zero code. The R4 page described it as "3 if any node was forced, else the first non-zero code"; the patch uses the latest, so the page and the patch differ for two non-orderly nodes with codes 4 and 1.

Findings (nothing changed; the coordinator decides):

- **3 beats 4.** In `4,0,3` and `4,3,4` the script exits 3 although a node reported a failed durability barrier or an unknown outcome. I think 4 should win over 3: 4 means the stop's durability barrier failed or no record proves what happened, 3 means the daemon left its stores untouched. With one blocked daemon, which is what O4 does, both rules give 3, so O4 is not affected. A one-line alternative is the numeric maximum, `[ "$rc" -le "$stop_rc" ] || stop_rc=$rc` in place of the `[ "$stop_rc" -eq 3 ] || stop_rc=$rc` line.
- **Order matters for rc 1.** `stop-file` exits 1 for `ERROR: ...` (for example `efsd PID N did not stop; retained, no SIGKILL attempted`, where the daemon may still be running). `1,0,4` exits 4 and `4,0,1` exits 1. The code alone cannot tell "a daemon is still running" from "stopped, outcome unknown"; the stderr line names the node and code. Under the numeric maximum rc 1 would lose to 3 and 4, so rc 1 needs its own rule if it should dominate.
- **The stderr line says `stopped` and `stores preserved` for rc 1 too,** which is not accurate for a daemon that was retained and kept running. Unchanged from the patch.
- A retained stale identity (`Stale daemon identity for PID N; process retained`) makes `stop-file` return 0, so the script prints `stopped (orderly)` although that process was not stopped. This was already so before the patch (it printed `stopped`).

## Callers and consumers

Searched `~/git/devops`, `~/git/cluster` and the efs repo for `stop.sh` and for the text `efs stopped`. The text `efs stopped` occurs only in `stop.sh` itself (line 96 and the comment at line 8). No file reads the exact success line. A second search for names built at run time (`${...}.sh`, `basename "$0"`, and the `portal.sh` dashboard) found only each sibling script re-executing itself over ssh, nothing that launches `stop.sh` indirectly.

| File and line | Use | Depends on the exact success line | Exit code under `set -e` |
|---|---|---|---|
| `devops/nuc-efs/env.sh:91-99` | multi-host `--clients-only` and `--daemons-only` dispatch | no | non-zero from a host fails the driver with exit 1 (`\|\| exit 1`, `\|\| rc=1`); the 3 or 4 is collapsed to 1, the host's stderr line still reaches the terminal |
| `devops/nuc-efs/stop.sh:9-11` | ssh re-exec | no | passes the remote status through |
| `devops/nuc-efs/deploy-and-restart.sh:28` | step 1 of the cycle, `set -e` | no | non-zero aborts the cycle before deploy, as before |
| `devops/nuc-efs/test_daemon_ownership.py:59` | the fixture | yes, now asserted (`stopped (orderly)`) | updated in this commit |
| `devops/README.md:28,37,247` | documentation of commands | no | none |
| `cluster/ORCD.md:98` | documentation of the orcd-mini lifecycle | no | none |
| `cluster/tests/test_cluster_stop.py`, `lib/client-stop.sh` | client stop only (also `tests/test_cluster_stop.py` in efs); not the devops `stop.sh` | no | none |
| `scripts/qualify_matrix.json:343` (orcd-mini), `:450` (intel), used by `scripts/qualify.py:350, 639, 2164` | `control.stop_cluster` | no | non-zero is a FAIL (`orderly cluster stop failed: exit 4: <last output line>`); stderr is merged into the log, so the new `NOT orderly` line becomes the message |
| `tests/measure/amd_query_cache_transition.py:35` | `subprocess.run(..., check=True)` | no | non-zero raises, as before |
| `scripts/evidence/orcd-mini/orcd_mini_c6.sh:24` | `./stop.sh 2>&1 \| tail -1` | prints the last line only | the pipeline status is `tail`'s, so a non-orderly stop is not an error there (an evidence script, unchanged by this patch's behaviour) |
| `scripts/evidence/orcd-mini/task_k_mini_deploy.sh:15` | `set -euo pipefail` | no | non-zero aborts, as before |
| `scripts/evidence/intel/intel_perf_round.sh:21` | `set -eu`, output to a log | no | non-zero aborts the round, as before |
| `scripts/evidence/k01/gates_nuc_k01_stop.sh` | uses `scripts/server.sh stop`, not the devops script | not a caller | none |

Required follow-ups: none. No caller reads the old success line, and every caller that checks the exit status already treated non-zero as a failure (the old script exited non-zero on a non-orderly node as well, only later and with fewer daemons stopped). No file in the efs repo was changed for this. Optional, outside the two allowed devops files: `env.sh:97` could pass the host's code through instead of `rc=1`.

## Test results

All runs are on this Mac (Darwin 25.6.0, Python 3.9, bash 3.2); no remote host and none of the stopped Tart VMs (efs1 to efs3) were used.

| Check | Before (devops `8bc872e`) | After (`f2a1fc5`) |
|---|---|---|
| `bash -n nuc-efs/stop.sh` | OK | OK |
| `python3 nuc-efs/test_daemon_ownership.py` | **NOTRUN on macOS:** exits 1 with `Run this ownership fixture on Linux with --efs-source pointing to EFS.` (the script guard `sys.platform != 'linux'`; the real `server_processes.py` needs `/proc` and `os.pidfd_open`) | same, NOTRUN on macOS; a Linux run is owed (phase 2, step 2) |
| `python3 nuc-efs/test_start_mode.py` | not run (does not touch `stop.sh`) | 2 lines PASS, integration part `requires Linux /proc: SKIP` |
| `python3 nuc-efs/test_efs_identity.py` | not run (does not touch `stop.sh`) | PASS |
| `python3 nuc-efs/test_gc_driver.py` | not run (does not touch `stop.sh`) | PASS |

Because the ownership fixture cannot run here, its flow was exercised on macOS with `scripts/evidence/k01/stop_sh_fixture_macos.sh`, which removes the Linux guard from a temporary copy of the test and uses the stand-in `scripts/evidence/k01/server_processes_macos_stub.py` as the helper. The stand-in checks the executable name `efsd` and the storage argument, sends SIGTERM, and reports orderly (rc 0) only when the exit record names that PID with `outcome=orderly`, else outcome unknown (rc 4). It does not check the barrier lines, the identity stamp or the forced record. This checks the test's logic, the C fake and `stop.sh`; it does not check the real helper.

| Test | `stop.sh` | Result |
|---|---|---|
| devops `HEAD` test (existing assertions only) | old | PASS |
| devops `HEAD` test | new | PASS (existing assertions kept) |
| new test | old | **FAIL** at the new orderly-line assertion: stdout was `intel-efs stopped` |
| new test | old with only the success text changed to `(orderly)` | **FAIL** at the non-orderly case: `daemon after the non-orderly one was not stopped` (n2 still running, `TimeoutExpired`) |
| new test | new | PASS |

## Reproduction

```sh
git -C ~/git/devops show devel:nuc-efs/stop.sh > /tmp/stop.old.sh
git -C ~/git/devops show k01b-stop-nonorderly:nuc-efs/stop.sh > /tmp/stop.new.sh
scripts/evidence/k01/stop_sh_exit_codes.sh /tmp/stop.old.sh   # table rows 10 to 12, before
scripts/evidence/k01/stop_sh_exit_codes.sh /tmp/stop.new.sh   # after
git -C ~/git/devops show devel:nuc-efs/test_daemon_ownership.py > /tmp/test.head.py
git -C ~/git/devops show k01b-stop-nonorderly:nuc-efs/test_daemon_ownership.py > /tmp/test.new.py
sed 's/efs stopped"/efs stopped (orderly)"/' /tmp/stop.old.sh > /tmp/stop.old-line.sh
for c in "test.head.py stop.old.sh" "test.head.py stop.new.sh" "test.new.py stop.old.sh" \
         "test.new.py stop.old-line.sh" "test.new.py stop.new.sh"; do
  set -- $c; scripts/evidence/k01/stop_sh_fixture_macos.sh /tmp/$1 /tmp/$2; done   # the five rows of the table above
cd ~/git/devops-k01b && for t in test_start_mode test_efs_identity test_gc_driver; do python3 nuc-efs/$t.py; done; bash -n nuc-efs/stop.sh
```

The Linux run of the ownership fixture is the open check: `python3 nuc-efs/test_daemon_ownership.py --efs-source <efs checkout>` on a Linux host with `gcc`.

## Phase 2 (not done)

Starts only on the coordinator's word that the user approved. O4 itself is not described as a runnable recipe anywhere; the pages say only "`stop.sh` qualification with a blocked handler". Two facts the run must settle first: the production `efsd` on orcd-mini has no fault hooks (`EFS_FAULT_SHUTDOWN_PARK` exists only in `EFS_FAULTS` builds, `tests/faults/efsd`), and the devops orcd-mini env manages three members, so `stop.sh`/`start.sh` do not touch n4 (see the [batch O page](orcd-mini-batch-o-20261109.md)).

```sh
# 1. Merge into devops devel (main checkout clean and on devel; fast-forward only)
git -C /Users/mike/git/devops merge --ff-only k01b-stop-nonorderly
# 2. Run the ownership fixture on a Linux host first (needs the user's OK for the host)
python3 nuc-efs/test_daemon_ownership.py --efs-source <efs checkout on that host>
# 3. Compare the deployed script with the old one before replacing it
ssh node9901 'sha256sum /data1/efs/devops/stop.sh'   # via the sanctioned ORCD socket
git -C /Users/mike/git/devops show 8bc872e:nuc-efs/stop.sh | shasum -a 256
# 4. Push only stop.sh (a regular file needs a DST ending in "/")
ECOPY_SSH_CMD='ssh -o ProxyCommand="ssh -W %h:%p orcd-login"' \
  /Users/mike/git/devops/bin/ecopy-push --no-delete /Users/mike/git/devops/nuc-efs/stop.sh erbmi1@node9901:/data1/efs/devops/
# 5. O4 on orcd-mini (stop both clients with plain client.sh stop first, then):
#    cd /data1/efs/devops && EFS_CLUSTER=orcd-mini ./stop.sh   # with one daemon's shutdown blocked
#    expect: every managed daemon stopped, stderr names the blocked node, exit 3
#    (forced) or 4, stores preserved, no "(orderly)" line; then restart on the preserved stores.
```

Step 5 is a sketch, not a tested procedure; its blocked-handler method has to be chosen and written down before the run (see the facts above).

## Not verified

- The ownership fixture against the real `scripts/server_processes.py` on Linux (macOS cannot run it).
- Any run of the patched `stop.sh` on a real daemon, mount or cluster; nothing was pushed to or run on any host.
- The multi-host (`intel`) driver path in `env.sh` with the new codes; only read.
