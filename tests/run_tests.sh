#!/bin/bash
# efs test orchestrator — runs the POSIX suites and the local-device
# benchmarks on the fcstor test nodes from the login node, collecting
# results into the git-tracked results/ tree.
#
# For efs write/read throughput use tests/stress/fio_honest_matrix.sh,
# NOT a time_based fio: --direct=1 skips only the kernel page cache, so a
# time_based run with no end_fsync measures the client's userspace dcache.
#
# Usage:
#   run_tests.sh posix [--keep] [--parallel] [efs-host ...]
#                                                POSIX suite vs XFS baseline
#   run_tests.sh posix2 [host-a] [host-b]        two-client visibility vs XFS
#   run_tests.sh posix2 multi                    4 non-overlapping pairs in parallel
#   run_tests.sh posixstress [N] [host ...]      N full suites per host in parallel
#                                                (default N=4, all 9 clients)
#   run_tests.sh posixpersist [--crash] [host ...] write, unmount, remount, verify
#                                                (durability across a remount)
#   run_tests.sh all [efs-host ...]              posix + nvme ceiling
#   run_tests.sh nvme [quick|full] [serial|parallel|both]
#                                                local NVMe ceiling on efsd servers
#   run_tests.sh meta [host]                     efs-bench --meta (1/4/16 workers)
#   run_tests.sh leaks [host]                    valgrind memcheck gate
#                                                (unit + efsd + efs-fuse)
#
# Env:
#   XFS_HOST (default node9901.ib)  XFS_DIR (default /data1/efs)
#   EFS_MNT  (default /tmp/efs-mount)
#   EFS_HOSTS default = fcstor007..015 (the 9 pure clients)
#   COMMIT=1 to git-commit the new results at the end.
#   POSIX_SSH_SEC (default 400)  POSIX2_STEP_SEC (default 45)
#   POSIX_TEST_SEC (default 15, per-test deadline in posix_suite.py)
#   POSIX_JOBS (default 16; isolated testdirs run concurrently)
#   POSIX_PER_HOST (default 1; run this many full-suite instances in parallel
#                  on EACH host — e.g. POSIX_PER_HOST=4 posix --parallel runs
#                  9 hosts x 4 = 36 concurrent suites. Each instance gets a
#                  unique --tag so their testdir prefixes/sweeps never collide.)
#   BUILD_SSH_SEC (default 60)  PERF_SSH_SEC (default 400)
#
# NOTE: must run with network/ssh access to the test nodes (outside the
# sandbox) because it shells out to efs-ssh for each node.
set -u
SSH="${EFS_SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
RESULTS="$REPO/results"
XFS_HOST=${XFS_HOST:-node9901.ib}
XFS_DIR=${XFS_DIR:-/data1/efs}
EFS_MNT=${EFS_MNT:-/tmp/efs-mount}
DEFAULT_HOSTS=(fcstor007.ib fcstor008.ib fcstor009.ib fcstor010.ib \
               fcstor011.ib fcstor012.ib fcstor013.ib fcstor014.ib fcstor015.ib)
NVME_HOSTS=(fcstor003.ib fcstor004.ib fcstor005.ib fcstor006.ib)
RUN_ID=$(date -u +%Y%m%d-%H%M%S)
POSIX_SSH_SEC=${POSIX_SSH_SEC:-400}
POSIX2_STEP_SEC=${POSIX2_STEP_SEC:-45}
BUILD_SSH_SEC=${BUILD_SSH_SEC:-60}
PERF_SSH_SEC=${PERF_SSH_SEC:-400}
PROBE_SSH_SEC=${PROBE_SSH_SEC:-10}

say() { echo "[run_tests] $*"; }

# Post-mkfs catchup: joiners rebuild shard tables for minutes. Starting
# posix/posixstress/posix2 in that window wedges warmups in D-state with
# used=0 and empty TSVs that look like lock saturation. Fail closed.
wait_cluster_idle() {
    local seed=${EFS_SEED:-172.16.223.57:19810}
    local primary=${EFS_PRIMARY:-fcstor003.ib}
    local i st idle gens ng
    say "wait_cluster_idle: Heal idle + matching gen on all nodes"
    for i in $(seq 1 45); do
        st=$(ssh_to 10 "$primary" "cd /tmp/efs && ./efs-mgmt status $seed" 2>/dev/null) || true
        idle=$(printf '%s\n' "$st" | grep -c 'idle' || true)
        gens=$(printf '%s\n' "$st" | sed -n 's/.*gen=\([0-9][0-9]*\).*/\1/p' | sort -u)
        ng=$(printf '%s\n' "$gens" | grep -c . || true)
        # Old CoW engine: matching gen= on every node. Raft status has no
        # gen= line. Four Heal-idle is ideal; three idle is the 005 gossip
        # DOWN STATUS probe after a bounce (process is alive).
        if { [ "$ng" = 1 ] && [ "$idle" -ge 4 ]; } ||
           { [ "$ng" = 0 ] && [ "$idle" -ge 3 ]; }; then
            say "  idle gen=$(printf '%s' "${gens:--}") (${i}s)"
            return 0
        fi
        sleep 2
    done
    say "  ERROR: cluster not idle after 90s (still in catchup) — refusing suite"
    printf '%s\n' "$st" | grep -E 'node |Heal|gen=' | head -20 >&2 || true
    return 1
}

# Every remote call has a hard deadline (efs-ssh default is 30s).
ssh_to() { # timeout_sec host [remote]
    local t=$1; shift
    EFS_SSH_TIMEOUT=$t "$SSH" "$@"
    local rc=$?
    if [ $rc -eq 124 ]; then
        say "TIMEOUT ${t}s: $*"
    fi
    return $rc
}

# rsync the tests dir to a node's /tmp/efs/tests
push_tests() { # host
    ssh_to 20 "$1" 'mkdir -p /tmp/efs && rsync -a --delete "$HOME/git/efs/tests/" /tmp/efs/tests/' \
        >/dev/null 2>&1
}

# Client env for efs-fuse. Empty EFS_RPC_PROF must not be exported — getenv
# treats "" as on (same trap as EFS_LOCK_PROF on the server).
fuse_client_env() {
    local e="EFS_TRANSPORT='${EFS_TRANSPORT:-}'"
    [ -n "${EFS_RPC_PROF:-}" ] && e="$e EFS_RPC_PROF=$EFS_RPC_PROF"
    printf '%s' "$e"
}

# mount a client if not already mounted (assumes a current efs-fuse binary).
# /proc/mounts lists the path at fuse_mount, before fuse_loop_mt is serving —
# require a successful stat so the caller does not use the mount too early.
ensure_mounted() { # host
    local h=$1
    local env
    env=$(fuse_client_env)
    if ssh_to "$PROBE_SSH_SEC" "$h" 'grep -q "efs-fuse /tmp/efs-mount " /proc/mounts && timeout 2 stat /tmp/efs-mount >/dev/null' 2>/dev/null; then
        # EFS_TRANSPORT is only read by the daemon at mount time, so reusing
        # an existing mount silently ignores it: asking for tcp on a cluster
        # whose clients are already up on auto keeps the RDMA mount, and the
        # run gets labelled with a transport it never used. Verify against
        # what the daemon actually reported and remount on a mismatch.
        # Only tcp is checked: the RDMA upgrade is lazy (it happens on the
        # first connection-pool checkout), so a fresh auto mount legitimately
        # has no "RDMA transport up" line yet and must not be remounted.
        if [ "${EFS_TRANSPORT:-}" = tcp ]; then
            local rdma_up
            # grep -c prints 0 and exits 1 on no match. `|| echo 0` then
            # concatenates to "00", which this check treats as RDMA and
            # remounts every TCP mount.
            rdma_up=$(ssh_to "$PROBE_SSH_SEC" "$h" \
                'grep -c "RDMA transport up" /tmp/efs/fuse.log 2>/dev/null || true' \
                2>/dev/null | tr -dc '0-9')
            if [ "${rdma_up:-0}" != "0" ]; then
                say "  $h: mounted on RDMA but EFS_TRANSPORT=tcp — remounting"
                remount_client "$h"
                return $?
            fi
        fi
        return 0
    fi
    say "  $h: mounting efs-fuse"
    local seed=${EFS_SEED:-172.16.223.57:19810}
    ssh_to "$BUILD_SSH_SEC" "$h" "cd /tmp/efs && [ -x ./efs-fuse ] || make efs-fuse >/dev/null 2>&1; \
        mkdir -p /tmp/efs-mount; \
        ($env setsid ./efs-fuse $seed efs-test /tmp/efs-mount >fuse.log 2>&1 </dev/null &); \
        for i in \$(seq 1 100); do sleep 0.15; \
            grep -q \"efs-fuse /tmp/efs-mount \" /proc/mounts || continue; \
            timeout 1 stat /tmp/efs-mount >/dev/null 2>&1 && exit 0; \
        done; echo mount-not-serving; tail -8 fuse.log; exit 1" 2>/dev/null
}

# Drop and remount one pure-client efs-fuse so it refetches the server snapshot.
# Do not use on a host that also runs efsd unless you intend to bounce FUSE only.
remount_client() { # host
    local h=$1
    local env
    env=$(fuse_client_env)
    say "  $h: remount efs-fuse"
    local seed=${EFS_SEED:-172.16.223.57:19810}
    ssh_to 15 "$h" "killall -9 efs-fuse 2>/dev/null || pkill -9 -x efs-fuse 2>/dev/null || true
        timeout 3 fusermount3 -uz /tmp/efs-mount 2>/dev/null || true
        cd /tmp/efs && mkdir -p /tmp/efs-mount && rm -f fuse.log
        $env setsid ./efs-fuse $seed efs-test /tmp/efs-mount >fuse.log 2>&1 </dev/null &
        for i in \$(seq 1 100); do
            sleep 0.15
            grep -q \"efs-fuse /tmp/efs-mount \" /proc/mounts || continue
            timeout 1 stat /tmp/efs-mount >/dev/null 2>&1 && exit 0
        done
        echo remount-timeout; tail -8 fuse.log; exit 1"
}

# Cleanly unmount one client: ask the kernel to unmount and let efs-fuse exit
# on its own, so it gets the chance to flush anything still dirty.
#
# Deliberately NOT remount_client, which SIGKILLs the daemon. The difference is
# the whole point of the durability gate: a clean unmount must preserve
# everything that was written, whereas a kill only has to preserve what was
# fsynced. Using the wrong one silently answers the wrong question.
#
# Never falls back to a kill -- if the daemon will not exit, the caller has to
# hear about it rather than get a crash test mislabelled as a clean unmount.
clean_unmount_client() { # host
    local h=$1
    say "  $h: clean unmount (no kill)"
    ssh_to 40 "$h" "fusermount3 -u /tmp/efs-mount 2>&1 || umount /tmp/efs-mount 2>&1 || true
        for i in \$(seq 1 100); do
            pgrep -x efs-fuse >/dev/null || exit 0
            sleep 0.2
        done
        echo 'efs-fuse still alive after unmount'; exit 1"
}

# rsync source + build efs-fuse + mount, per client (setup before perf multi)
cmd_setup() { # [host ...]
    local hosts=("$@")
    [ ${#hosts[@]} -eq 0 ] && hosts=("${DEFAULT_HOSTS[@]}")
    say "setup: rsync+build+mount on: ${hosts[*]}"
    local pids=()
    for h in "${hosts[@]}"; do
        ( ssh_to "$BUILD_SSH_SEC" "$h" 'rsync -a --delete --exclude="/mnt/" --exclude="/mnt-s3/" --exclude="/mnt-cold/" --exclude="*.log" \
              "$HOME/git/efs/" /tmp/efs/ >/dev/null 2>&1 && \
              cd /tmp/efs && make efs-fuse >/dev/null 2>&1' && \
          ensure_mounted "$h" && \
          ssh_to "$PROBE_SSH_SEC" "$h" 'grep -q "efs-fuse /tmp/efs-mount " /proc/mounts' && \
          echo "  $h: ready" || echo "  $h: SETUP FAILED" ) &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p"; done
}

# ---------------------------------------------------------------- posix ---
cmd_posix() { # [--keep] [--parallel] [efs-host ...]
    local keep="" parallel=0 hosts=()
    for a in "$@"; do
        case "$a" in
            --keep) keep="--keep" ;;
            --parallel) parallel=1 ;;
            *) hosts+=("$a") ;;
        esac
    done
    [ ${#hosts[@]} -eq 0 ] && hosts=("${DEFAULT_HOSTS[@]:0:1}")
    local pdir="$RESULTS/posix/$RUN_ID"
    mkdir -p "$pdir"

    wait_cluster_idle || return 1

    say "posix: XFS baseline on $XFS_HOST:$XFS_DIR"
    push_tests "$XFS_HOST"
    local xfs_tsv="/tmp/posix-xfs-$RUN_ID.tsv"
    ssh_to "$POSIX_SSH_SEC" "$XFS_HOST" "rm -f '$xfs_tsv'
        PYTHONUNBUFFERED=1 timeout -k 5 $((POSIX_SSH_SEC - 15)) \
        python3 /tmp/efs/tests/posix/posix_suite.py '$XFS_DIR' \
        --timeout-s ${POSIX_TEST_SEC:-15} --jobs ${POSIX_JOBS:-16} \
        --results '$xfs_tsv'
        if [ ! -s '$xfs_tsv' ]; then
            printf '%s\\n' '# TIMEOUT no TSV' 'test	result	detail' \
                '# summary pass=0 fail=0 skip=0 total=0 dur=0'
            exit 124
        fi
        cat '$xfs_tsv'" \
        > "$pdir/xfs-baseline.tsv"
    say "  baseline: $(grep -c $'\tPASS' "$pdir/xfs-baseline.tsv") pass"

    # One suite instance on a host. With POSIX_PER_HOST>1 the instance gets a
    # unique --tag so its per-host testdir prefix and startup sweep never touch
    # a concurrently-running sibling's tree on the same host.
    posix_instance() { # host idx per
        local h=$1 i=$2 per=$3
        local tag="" suffix=""
        if [ "$per" -gt 1 ]; then
            tag="--tag $i"
            suffix="-$i"
        fi
        local remote_tsv="/tmp/posix-efs-${RUN_ID}-${h%.ib}${suffix}.tsv"
        ssh_to "$POSIX_SSH_SEC" "$h" "rm -f '$remote_tsv'
            PYTHONUNBUFFERED=1 timeout -k 5 $((POSIX_SSH_SEC - 15)) \
            python3 /tmp/efs/tests/posix/posix_suite.py '$EFS_MNT' \
            $keep $tag --timeout-s ${POSIX_TEST_SEC:-15} --jobs ${POSIX_JOBS:-16} \
            --results '$remote_tsv'
            if [ ! -s '$remote_tsv' ]; then
                printf '%s\\n' '# TIMEOUT no TSV' 'test\tresult\tdetail' \
                    '# summary pass=0 fail=0 skip=0 total=0 dur=0'
                exit 124
            fi
            cat '$remote_tsv'" \
            > "$pdir/efs-${h%.ib}${suffix}.tsv"
        if [ "$per" -le 1 ]; then
            say "  --- compare $h vs XFS baseline ---"
            python3 "$REPO/tests/posix/compare.py" "$pdir/xfs-baseline.tsv" \
                "$pdir/efs-${h%.ib}${suffix}.tsv" | tee "$pdir/compare-${h%.ib}${suffix}.txt"
            return ${PIPESTATUS[0]}
        fi
        python3 "$REPO/tests/posix/compare.py" "$pdir/xfs-baseline.tsv" \
            "$pdir/efs-${h%.ib}${suffix}.tsv" > "$pdir/compare-${h%.ib}${suffix}.txt" 2>/dev/null
        local crc=$?
        local bp bugs
        bp=$(awk '/both pass/{print $4; exit}' "$pdir/compare-${h%.ib}${suffix}.txt")
        bugs=$(awk '/EFS BUGS/{print $4; exit}' "$pdir/compare-${h%.ib}${suffix}.txt")
        say "  ${h%.ib}${suffix}: ${bp:-0} both-pass, ${bugs:-0} EFS-bugs"
        return $crc
    }

    posix_one() { # host
        local h=$1
        local per=${POSIX_PER_HOST:-1}
        say "posix: efs on $h:$EFS_MNT $keep (x$per)"
        push_tests "$h"
        # Guard: a dead efs-fuse leaves /tmp/efs-mount as a plain local dir and
        # the suite would silently pass against tmpfs (0 "EFS bugs"). Refuse
        # to run unless EFS_MNT is a live fuse.efs-fuse mount.
        ssh_to 15 "$h" "findmnt -n -o FSTYPE '$EFS_MNT' | grep -q '^fuse\.efs-fuse$'" \
            || { say "  ERROR: $h:$EFS_MNT is not a live efs-fuse mount — skipping (run setup)"; \
                 printf '# ERROR not an efs-fuse mount\ntest\tresult\tdetail\n# summary pass=0 fail=0 skip=0 total=0 dur=0\n' \
                     > "$pdir/efs-${h%.ib}.tsv"; \
                 python3 "$REPO/tests/posix/compare.py" "$pdir/xfs-baseline.tsv" \
                     "$pdir/efs-${h%.ib}.tsv" > "$pdir/compare-${h%.ib}.txt" 2>/dev/null; \
                 return 1; }
        # Warmup: confirm the mount serves ops (not just present in /proc/mounts).
        # Failure is a FAIL — a WARN+continue in the catchup window produced
        # empty TSVs that looked like lock saturation.
        ssh_to 70 "$h" 'for i in $(seq 1 20); do
            if timeout 3 python3 -c "
import os, tempfile, shutil
d = tempfile.mkdtemp(prefix=\"warmup-\", dir=\"/tmp/efs-mount\")
p = os.path.join(d, \"f\"); open(p, \"w\").write(\"x\")
assert open(p).read() == \"x\"
shutil.rmtree(d)
" 2>/dev/null; then exit 0; fi
            sleep 0.5
        done; echo "  ERROR: '"$h"' warmup did not converge" >&2; exit 1' \
            || { say "  ERROR: $h warmup failed — not starting suite"; return 1; }
        # Run $per full-suite instances in parallel on this host.
        local pids=() rc=0 i p
        for i in $(seq 0 $((per - 1))); do
            posix_instance "$h" "$i" "$per" &
            pids+=($!)
        done
        for p in "${pids[@]}"; do
            wait "$p" || rc=1
        done
        return $rc
    }

    local rc=0
    if [ "$parallel" = 1 ]; then
        local pids=()
        for h in "${hosts[@]}"; do
            posix_one "$h" &
            pids+=($!)
        done
        for p in "${pids[@]}"; do
            wait "$p" || rc=1
        done
    else
        for h in "${hosts[@]}"; do
            posix_one "$h" || rc=1
        done
    fi
    say "posix results in $pdir"
    return $rc
}

# -------------------------------------------------------- posixpersist ---
# Durability gate: write on a mount, tear the mount down, bring it back, and
# check the writes are still there. Every other suite writes and verifies
# inside one mount session, so all of them would pass even if efs kept
# everything in client memory.
cmd_posixpersist() { # [--keep] [--crash] [efs-host ...]
    local keep="" mode=clean hosts=()
    for a in "$@"; do
        case "$a" in
            --keep) keep="--keep" ;;
            # --crash SIGKILLs the daemon instead of unmounting it. Then only
            # fsynced data is required to survive, so expect the un-fsynced
            # tests to fail; that is the POSIX contract, not a bug.
            --crash) mode=crash ;;
            *) hosts+=("$a") ;;
        esac
    done
    [ ${#hosts[@]} -eq 0 ] && hosts=("${DEFAULT_HOSTS[@]:0:1}")
    local pdir="$RESULTS/posixpersist/$RUN_ID"
    mkdir -p "$pdir"
    local py=/tmp/efs/tests/posix/posix_persist.py
    local sec=${PERSIST_SSH_SEC:-600}
    local rc=0

    for h in "${hosts[@]}"; do
        local short=${h%.ib}
        say "posixpersist: $h:$EFS_MNT (unmount mode: $mode)"
        push_tests "$h"
        ensure_mounted "$h" || { say "  $h: not mounted"; rc=1; continue; }
        # Same guard as cmd_posix: a dead efs-fuse leaves EFS_MNT as a plain
        # local directory, and a durability suite would then be testing the
        # local disk's durability rather than efs's.
        ssh_to 15 "$h" "findmnt -n -o FSTYPE '$EFS_MNT' | grep -q '^fuse\.efs-fuse$'" \
            || { say "  ERROR: $h:$EFS_MNT is not a live efs-fuse mount — run setup"; rc=1; continue; }

        # Record which daemon wrote the data, so we can prove it is gone.
        local pid_before
        pid_before=$(ssh_to 15 "$h" 'pgrep -x efs-fuse | head -1' | tr -d '[:space:]')

        say "  phase 1/2: prepare (writing)"
        ssh_to "$sec" "$h" "rm -f /tmp/persist-prep.tsv
            PYTHONUNBUFFERED=1 timeout -k 5 $((sec - 20)) \
            python3 $py '$EFS_MNT' --phase prepare \
            --timeout-s ${PERSIST_TEST_SEC:-60} --results /tmp/persist-prep.tsv
            cat /tmp/persist-prep.tsv 2>/dev/null" > "$pdir/prepare-$short.tsv"
        local pfail
        pfail=$(awk -F'fail=' '/# summary/{split($2,a," "); print a[1]}' \
                "$pdir/prepare-$short.tsv")
        say "  prepare: $(grep -c $'\tPASS' "$pdir/prepare-$short.tsv") wrote, ${pfail:-?} failed"
        if [ "${pfail:-1}" != "0" ]; then
            say "  NOTE: prepare had failures — verify results below are only"
            say "        meaningful for the tests that prepared cleanly"
        fi

        if [ "$mode" = crash ]; then
            # kill -9: only fsynced data is required to survive.
            say "  CRASH mode: SIGKILL efs-fuse (only fsynced data need survive)"
            remount_client "$h" || { say "  $h: remount failed"; rc=1; continue; }
        else
            # Clean unmount: everything written must survive, fsynced or not.
            clean_unmount_client "$h" \
                || { say "  $h: clean unmount failed — refusing to fall back to a kill"; rc=1; continue; }
            ensure_mounted "$h" || { say "  $h: remount failed"; rc=1; continue; }
        fi

        # The whole gate rests on the mount really having gone away. If the
        # daemon survived, its page cache could serve the reads and every test
        # would pass without anything having been made durable.
        local pid_after
        pid_after=$(ssh_to 15 "$h" 'pgrep -x efs-fuse | head -1' | tr -d '[:space:]')
        if [ -z "$pid_after" ] || [ "$pid_before" = "$pid_after" ]; then
            say "  ERROR: efs-fuse pid did not change ($pid_before -> ${pid_after:-none});"
            say "         the mount was not actually torn down — result would be meaningless"
            rc=1
            continue
        fi
        say "  efs-fuse restarted: pid $pid_before -> $pid_after"

        say "  phase 2/2: verify (reading back after remount)"
        ssh_to "$sec" "$h" "rm -f /tmp/persist-ver.tsv
            PYTHONUNBUFFERED=1 timeout -k 5 $((sec - 20)) \
            python3 $py '$EFS_MNT' --phase verify $keep \
            --timeout-s ${PERSIST_TEST_SEC:-60} --results /tmp/persist-ver.tsv
            cat /tmp/persist-ver.tsv 2>/dev/null" > "$pdir/verify-$short.tsv"

        local vpass vfail
        vpass=$(grep -c $'\tPASS' "$pdir/verify-$short.tsv")
        vfail=$(grep -c $'\tFAIL' "$pdir/verify-$short.tsv")
        say "  --- $short: survived the remount: $vpass, LOST: $vfail ---"
        if [ "${vfail:-0}" != "0" ]; then
            grep $'\tFAIL' "$pdir/verify-$short.tsv" \
                | awk -F'\t' '{printf "    DATA LOSS  %-28s %s\n", $1, $3}'
            rc=1
        fi
    done
    say "results in $pdir"
    return $rc
}

# ----------------------------------------------------------- posix2 -----
# Two efs-fuse clients, same export. Prepare testdirs on A, remount B so
# both see the parent, then mutate on A and check B with no further remount.
# XFS baseline uses the same directory as both sides (kernel namespace).
posix2_one_pair() { # host-a host-b results-dir parent
    local host_a=$1 host_b=$2 pdir=$3 parent=$4
    local py="$REPO/tests/posix/posix_2client.py"
    say "posix2: efs A=$host_a B=$host_b parent=$parent"
    ensure_mounted "$host_a" || { say "  $host_a not mounted"; return 1; }
    ensure_mounted "$host_b" || { say "  $host_b not mounted"; return 1; }
    # Unique parent per pair so a leftover posix-2c (EEXIST/EIO/false
    # flock) cannot leak across parallel pairs. Remount A+B around
    # prepare so B does not walk an old ino of the same name.
    remount_client "$host_a" || return 1
    remount_client "$host_b" || return 1
    ssh_to 60 "$host_a" "python3 '$py' --prepare '$EFS_MNT' --parent '$parent'" || {
        say "  prepare failed on $host_a"
        return 1
    }
    say "  remount $host_b after prepare"
    remount_client "$host_b" || return 1
    ssh_to 20 "$host_b" "timeout -k 2 15 test -d '$EFS_MNT/$parent'" || {
        say "  $host_b still cannot see $EFS_MNT/$parent after remount"
        return 1
    }
    EFS_SSH_TIMEOUT=$POSIX2_STEP_SEC POSIX2_STEP_SEC=$POSIX2_STEP_SEC \
        python3 "$py" --remote "$host_a" "$host_b" --mnt "$EFS_MNT" \
        --parent "$parent" \
        --results "$pdir/efs-${host_a%.ib}-${host_b%.ib}.tsv"
    local rc=$?
    ssh_to 10 "$host_a" "timeout -k 2 8 rm -rf '$EFS_MNT/$parent'" || true
    say "  --- compare $host_a/$host_b vs XFS ---"
    python3 "$REPO/tests/posix/compare.py" "$pdir/xfs-baseline.tsv" \
        "$pdir/efs-${host_a%.ib}-${host_b%.ib}.tsv" | \
        tee "$pdir/compare-${host_a%.ib}-${host_b%.ib}.txt"
    [ ${PIPESTATUS[0]} -ne 0 ] && rc=1
    return $rc
}

cmd_posix2() { # [host-a] [host-b]  |  multi
    local pdir="$RESULTS/posix2/$RUN_ID"
    mkdir -p "$pdir" "$RESULTS/posix2"
    local py="$REPO/tests/posix/posix_2client.py"

    wait_cluster_idle || return 1

    say "posix2: XFS baseline on $XFS_HOST:$XFS_DIR (same path twice)"
    push_tests "$XFS_HOST"
    ssh_to "$POSIX_SSH_SEC" "$XFS_HOST" "timeout -k 5 $((POSIX_SSH_SEC - 15)) \
        python3 /tmp/efs/tests/posix/posix_2client.py --local \
        '$XFS_DIR' '$XFS_DIR' --results /tmp/posix2-xfs.tsv; \
        cat /tmp/posix2-xfs.tsv" > "$pdir/xfs-baseline.tsv"
    say "  baseline: $(grep -c $'\tPASS' "$pdir/xfs-baseline.tsv") pass"

    if [ "${1:-}" = "multi" ]; then
        # Non-overlapping hosts so 4 pairs can run at once.
        local pairs=(
            "fcstor007.ib fcstor008.ib"
            "fcstor009.ib fcstor012.ib"
            "fcstor010.ib fcstor013.ib"
            "fcstor011.ib fcstor015.ib"
        )
        local pids=() rc=0
        for pair in "${pairs[@]}"; do
            local ha=${pair%% *} hb=${pair#* }
            local parent="posix-2c-${ha%.ib}-${hb%.ib}-${RUN_ID}"
            posix2_one_pair "$ha" "$hb" "$pdir" "$parent" &
            pids+=($!)
        done
        for p in "${pids[@]}"; do
            wait "$p" || rc=1
        done
        say "posix2 results in $pdir"
        return $rc
    fi

    local host_a=${1:-fcstor007.ib}
    local host_b=${2:-fcstor008.ib}
    local parent="posix-2c-${host_a%.ib}-${host_b%.ib}-${RUN_ID}"
    posix2_one_pair "$host_a" "$host_b" "$pdir" "$parent"
    local rc=$?
    say "posix2 results in $pdir"
    return $rc
}

# ----------------------------------------------------------- local nvme ---
# Ceiling of the 4 efsd servers' /data1/01..06 NVMe mounts. Data lands in
# <path>/fio-ceil only (never <path>/efs). efsd stays up; idle metadata
# I/O is negligible next to the fio working set.
cmd_nvme() { # [quick|full] [serial|parallel|both]
    local mode=full layout=both
    for a in "$@"; do
        case "$a" in
            quick|full) mode=$a ;;
            serial|parallel|both) layout=$a ;;
            *) say "nvme: unknown arg $a"; return 2 ;;
        esac
    done
    local pdir="$RESULTS/nvme/$RUN_ID"
    mkdir -p "$pdir" "$RESULTS/nvme"
    touch "$RESULTS/nvme/history.tsv"
    [ -s "$RESULTS/nvme/history.tsv" ] || \
        echo -e "run_id\thost\tpath\tlayout\tsuite\ttest\tbw_mib_s\tn" \
            >"$RESULTS/nvme/history.tsv"
    say "nvme ($mode, $layout) on: ${NVME_HOSTS[*]}"

    local h pids=()
    for h in "${NVME_HOSTS[@]}"; do
        (
            push_tests "$h"
            ssh_to "$PERF_SSH_SEC" "$h" "timeout -k 10 $((PERF_SSH_SEC - 20)) \
                bash /tmp/efs/tests/perf/perf_local_nvme.sh \
                /tmp/perf-nvme-$RUN_ID.tsv '$mode' '$layout'; \
                cat /tmp/perf-nvme-$RUN_ID.tsv"
        ) > "$pdir/nvme-${h%.ib}.tsv" 2>"$pdir/nvme-${h%.ib}.log" &
        pids+=($!)
    done
    local rc=0
    for p in "${pids[@]}"; do wait "$p" || rc=1; done

    local summary="$pdir/summary.tsv"
    {
        echo -e "run_id\thost\tpath\tlayout\tsuite\ttest\tbw_mib_s\tiops\trc"
        for h in "${NVME_HOSTS[@]}"; do
            awk -v rid="$RUN_ID" -F'\t' '
                NR==1 { next }
                $1 ~ /^PERF_LOCAL_NVME_DONE/ { next }
                NF>=9 { print rid"\t"$2"\t"$3"\t"$4"\t"$5"\t"$6"\t"$7"\t"$8"\t"$9 }
            ' "$pdir/nvme-${h%.ib}.tsv" 2>/dev/null
        done
    } > "$summary"
    awk -F'\t' 'NR>1 && NF>=9 {
            k=$2"\t"$3"\t"$4"\t"$5"\t"$6; bw[k]+=$7; n[k]++
        }
        END { for (k in bw) printf "%s\t%s\t%.1f\t%d\n", "'"$RUN_ID"'", k, bw[k], n[k] }' \
        "$summary" >> "$RESULTS/nvme/history.tsv"
    say "nvme results in $pdir ; history appended"
    echo "--- per-drive serial sw-1m / sr-1m / rw-4k ---"
    awk -F'\t' 'NR>1 && $4=="serial" && $6 ~ /^(sw-1m|sr-1m|rw-4k)$/ {
            printf "%s  %s  %-6s  %8s\n", $2, $3, $6, $7
        }' "$summary" | sort
    echo "--- host-sum parallel (all 6 drives busy) ---"
    awk -F'\t' 'NR>1 && $4=="parallel" && $6 ~ /^(sw-1m|sr-1m|rw-4k)$/ {
            k=$2"\t"$6; bw[k]+=$7
        }
        END { for (k in bw) printf "%s  %8.1f\n", k, bw[k] }' "$summary" | sort
    return $rc
}

# ----------------------------------------------------------------- meta ---
# efs-bench --meta on one client (workers 1/4/16). Build efs-bench on the node.
cmd_meta() { # [host]
    local host=${1:-fcstor007.ib}
    local pdir="$RESULTS/meta/$RUN_ID"
    mkdir -p "$pdir" "$RESULTS/meta"
    say "meta: efs-bench --meta on $host (workers 1/4/16)"
    ssh_to "$BUILD_SSH_SEC" "$host" 'rsync -a --delete --exclude="/mnt/" --exclude="*.log" \
        "$HOME/git/efs/" /tmp/efs/ && cd /tmp/efs && make efs-bench' \
        >"$pdir/build.log" 2>&1 || { say "  build failed"; cat "$pdir/build.log"; return 1; }
    local w out
    : > "$pdir/summary.tsv"
    echo -e "run_id\thost\tworkers\tphase\trw\tops\twall_s\tops_s" \
        > "$pdir/summary.tsv"
    if [ ! -s "$RESULTS/meta/history.tsv" ]; then
        echo -e "run_id\thost\tworkers\tphase\trw\tops\twall_s\tops_s" \
            > "$RESULTS/meta/history.tsv"
    fi
    for w in 1 4 16; do
        out="$pdir/meta-${host%.ib}-w${w}.txt"
        say "  workers=$w"
        ssh_to 120 "$host" "$(fuse_client_env) cd /tmp/efs && timeout -k 5 110 \
            ./efs-bench 172.16.223.57:19810 --meta \
            --export efs-test --files 5000 --dirs 64 --workers $w" \
            | tee "$out"
        awk -v rid="$RUN_ID" -v host="${host%.ib}" -v w="$w" '
            $1=="BENCH_OK" && $2=="kind=meta" {
                ph=rw=ops=wall=ops_s=""
                for (i=3;i<=NF;i++) {
                    split($i, a, "=")
                    if (a[1]=="phase") ph=a[2]
                    if (a[1]=="rw") rw=a[2]
                    if (a[1]=="ops") ops=a[2]
                    if (a[1]=="wall_s") wall=a[2]
                    if (a[1]=="ops_s") ops_s=a[2]
                }
                if (ph!="") {
                    line=rid"\t"host"\t"w"\t"ph"\t"rw"\t"ops"\t"wall"\t"ops_s
                    print line
                }
            }' "$out" | tee -a "$pdir/summary.tsv" >> "$RESULTS/meta/history.tsv"
    done
    say "meta results in $pdir"
    echo "--- meta ops/s ---"
    awk -F'\t' 'NR>1 { printf "  w=%-3s  %-8s %-5s  %8s ops  %8s ops/s\n", $3,$4,$5,$6,$8 }' \
        "$pdir/summary.tsv"
}

cmd_leaks() { # [host]
    local host=${1:-fcstor003.ib}
    local pdir="$RESULTS/leaks/$RUN_ID"
    mkdir -p "$pdir" "$RESULTS/leaks"
    say "leaks: valgrind memcheck gate on $host (unit + efsd + efs-fuse)"
    ssh_to "$BUILD_SSH_SEC" "$host" 'mkdir -p /tmp/efs && rsync -a --delete \
        --exclude="/mnt/" --exclude="*.log" --exclude="results/" \
        "$HOME/git/efs/" /tmp/efs/' >"$pdir/rsync.log" 2>&1 \
        || { say "  rsync failed"; cat "$pdir/rsync.log"; return 1; }
    # valgrind is ~20-50x slower; the whole gate is a few minutes.
    ssh_to 600 "$host" 'cd /tmp/efs && bash tests/valgrind_leaks.sh /tmp/efs-vg' \
        2>&1 | tee "$pdir/leaks.txt"
    local rc=${PIPESTATUS[0]}
    if [ $rc -eq 0 ]; then
        say "leaks PASS — results in $pdir/leaks.txt"
    else
        say "leaks FAIL (rc=$rc) — see $pdir/leaks.txt"
    fi
    return $rc
}

# ------------------------------------------------------------- posixstress ---
# High-parallelism stress: N full posix suites per host, all hosts in parallel.
# Default 9 clients x 4 = 36 concurrent suite instances. Each instance gets a
# unique --tag so its testdir prefix/sweep never collides with a sibling.
cmd_posixstress() { # [N] [efs-host ...]
    local n=4
    if [ $# -gt 0 ] && [[ "$1" =~ ^[0-9]+$ ]]; then n=$1; shift; fi
    local hosts=("$@")
    [ ${#hosts[@]} -eq 0 ] && hosts=("${DEFAULT_HOSTS[@]}")
    say "posixstress: ${#hosts[@]} hosts x $n suites/host = $(( ${#hosts[@]} * n )) instances"
    POSIX_PER_HOST=$n cmd_posix --parallel "${hosts[@]}"
}

# ------------------------------------------------------------------ all ---
cmd_all() { # [efs-host ...]
    local hosts=("$@")
    [ ${#hosts[@]} -eq 0 ] && hosts=("${DEFAULT_HOSTS[@]}")
    cmd_posix "${hosts[@]}"
    cmd_nvme
}

main() {
    mkdir -p "$RESULTS/posix" "$RESULTS/posix2" "$RESULTS/perf" "$RESULTS/nvme" \
             "$RESULTS/meta" "$RESULTS/leaks"
    local cmd=${1:-}
    shift || true
    case "$cmd" in
        posix) cmd_posix "$@" ;;
        posixstress) cmd_posixstress "$@" ;;
        posixpersist) cmd_posixpersist "$@" ;;
        posix2) cmd_posix2 "$@" ;;
        nvme)  cmd_nvme "$@" ;;
        meta) cmd_meta "$@" ;;
        leaks) cmd_leaks "$@" ;;
        setup) cmd_setup "$@" ;;
        all)   cmd_all "$@" ;;
        *) sed -n '2,23p' "$0"; return 2 ;;
    esac
    local rc=$?
    if [ "${COMMIT:-0}" = 1 ]; then
        ( cd "$REPO" && git add results && \
          git commit -q -m "test results $RUN_ID ($cmd)" && \
          say "committed results $RUN_ID" )
    fi
    return $rc
}
main "$@"
