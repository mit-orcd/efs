#!/bin/bash
# efs test orchestrator — runs the POSIX + perf suites on the fcstor test
# nodes from the login node, collects results into the git-tracked results/
# tree, and appends to results/perf/history.tsv for trend tracking.
#
# Usage:
#   run_tests.sh posix [efs-host ...]            POSIX suite vs XFS baseline
#   run_tests.sh posix2 [host-a] [host-b]        two-client visibility vs XFS
#   run_tests.sh perf single <host> [quick|full] perf on one client
#   run_tests.sh perf multi [host ...] [quick|full]
#                                                perf across clients (parallel)
#   run_tests.sh all [efs-host ...]              posix + perf multi
#   run_tests.sh nvme [quick|full] [serial|parallel|both]
#                                                local NVMe ceiling on efsd servers
#   run_tests.sh ewrite [host]                   30s ewrite.sh sweep (1 2/4/8/16)
#   run_tests.sh meta [host]                     efs-bench --meta (1/4/16 workers)
#
# Env:
#   XFS_HOST (default node9901.ib)  XFS_DIR (default /data1/efs)
#   EFS_MNT  (default /tmp/efs/mnt)
#   EFS_HOSTS default = fcstor007..015 (the 9 pure clients)
#   COMMIT=1 to git-commit the new results at the end.
#
# NOTE: must run with network/ssh access to the test nodes (outside the
# sandbox) because it shells out to efs-ssh for each node.
set -u
SSH="${EFS_SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
RESULTS="$REPO/results"
XFS_HOST=${XFS_HOST:-node9901.ib}
XFS_DIR=${XFS_DIR:-/data1/efs}
EFS_MNT=${EFS_MNT:-/tmp/efs/mnt}
DEFAULT_HOSTS=(fcstor007.ib fcstor008.ib fcstor009.ib fcstor010.ib \
               fcstor011.ib fcstor012.ib fcstor013.ib fcstor014.ib fcstor015.ib)
NVME_HOSTS=(fcstor003.ib fcstor004.ib fcstor005.ib fcstor006.ib)
RUN_ID=$(date -u +%Y%m%d-%H%M%S)

say() { echo "[run_tests] $*"; }

# rsync the tests dir to a node's /tmp/efs/tests
push_tests() { # host
    $SSH "$1" 'mkdir -p /tmp/efs && rsync -a --delete "$HOME/git/efs/tests/" /tmp/efs/tests/' \
        >/dev/null 2>&1
}

# mount a client if not already mounted (assumes a current efs-fuse binary)
ensure_mounted() { # host
    local h=$1
    if $SSH "$h" 'grep -q "efs-fuse /tmp/efs/mnt " /proc/mounts' 2>/dev/null; then
        return 0
    fi
    say "  $h: mounting efs-fuse"
    $SSH "$h" 'cd /tmp/efs && [ -x ./efs-fuse ] || make efs-fuse >/dev/null 2>&1; \
        mkdir -p /tmp/efs/mnt; \
        (setsid ./efs-fuse 172.16.223.57:19810 efs-test /tmp/efs/mnt >fuse.log 2>&1 </dev/null &); \
        sleep 5; grep -q "efs-fuse /tmp/efs/mnt " /proc/mounts' 2>/dev/null
}

# Drop and remount one pure-client efs-fuse so it refetches the server snapshot.
# Do not use on a host that also runs efsd unless you intend to bounce FUSE only.
remount_client() { # host
    local h=$1
    say "  $h: remount efs-fuse"
    $SSH "$h" 'fusermount3 -uz /tmp/efs/mnt 2>/dev/null; pkill -x efs-fuse; sleep 0.4
        cd /tmp/efs && mkdir -p /tmp/efs/mnt
        setsid ./efs-fuse 172.16.223.57:19810 efs-test /tmp/efs/mnt >fuse.log 2>&1 </dev/null &
        for i in $(seq 1 20); do
            sleep 0.5
            grep -q "efs-fuse /tmp/efs/mnt " /proc/mounts && exit 0
        done
        echo remount-timeout; tail -8 fuse.log; exit 1'
}

# rsync source + build efs-fuse + mount, per client (setup before perf multi)
cmd_setup() { # [host ...]
    local hosts=("$@")
    [ ${#hosts[@]} -eq 0 ] && hosts=("${DEFAULT_HOSTS[@]}")
    say "setup: rsync+build+mount on: ${hosts[*]}"
    local pids=()
    for h in "${hosts[@]}"; do
        ( $SSH "$h" 'rsync -a --delete --exclude="/mnt/" --exclude="/mnt-s3/" --exclude="/mnt-cold/" --exclude="*.log" \
              "$HOME/git/efs/" /tmp/efs/ >/dev/null 2>&1 && \
              cd /tmp/efs && make efs-fuse >/dev/null 2>&1' && \
          ensure_mounted "$h" && \
          $SSH "$h" 'grep -q "efs-fuse /tmp/efs/mnt " /proc/mounts' && \
          echo "  $h: ready" || echo "  $h: SETUP FAILED" ) &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p"; done
}

# ---------------------------------------------------------------- posix ---
cmd_posix() { # [efs-host ...]
    local hosts=("$@")
    [ ${#hosts[@]} -eq 0 ] && hosts=("${DEFAULT_HOSTS[@]:0:1}")
    local pdir="$RESULTS/posix/$RUN_ID"
    mkdir -p "$pdir"

    say "posix: XFS baseline on $XFS_HOST:$XFS_DIR"
    push_tests "$XFS_HOST"
    $SSH "$XFS_HOST" "python3 /tmp/efs/tests/posix/posix_suite.py '$XFS_DIR' \
        --results /tmp/posix-xfs.tsv >/dev/null 2>&1; cat /tmp/posix-xfs.tsv" \
        > "$pdir/xfs-baseline.tsv"
    say "  baseline: $(grep -c $'\tPASS' "$pdir/xfs-baseline.tsv") pass"

    local rc=0
    for h in "${hosts[@]}"; do
        say "posix: efs on $h:$EFS_MNT"
        push_tests "$h"
        $SSH "$h" "python3 /tmp/efs/tests/posix/posix_suite.py '$EFS_MNT' \
            --results /tmp/posix-efs.tsv >/dev/null 2>&1; cat /tmp/posix-efs.tsv" \
            > "$pdir/efs-${h%.ib}.tsv"
        say "  --- compare $h vs XFS baseline ---"
        python3 "$REPO/tests/posix/compare.py" "$pdir/xfs-baseline.tsv" \
            "$pdir/efs-${h%.ib}.tsv" | tee "$pdir/compare-${h%.ib}.txt"
        [ ${PIPESTATUS[0]} -ne 0 ] && rc=1
    done
    say "posix results in $pdir"
    return $rc
}

# ----------------------------------------------------------- posix2 -----
# Two efs-fuse clients, same export. Prepare testdirs on A, remount B so
# both see the parent, then mutate on A and check B with no further remount.
# XFS baseline uses the same directory as both sides (kernel namespace).
cmd_posix2() { # [host-a] [host-b]
    local host_a=${1:-fcstor007.ib}
    local host_b=${2:-fcstor008.ib}
    local pdir="$RESULTS/posix2/$RUN_ID"
    mkdir -p "$pdir" "$RESULTS/posix2"
    local py="$REPO/tests/posix/posix_2client.py"

    say "posix2: XFS baseline on $XFS_HOST:$XFS_DIR (same path twice)"
    push_tests "$XFS_HOST"
    $SSH "$XFS_HOST" "python3 /tmp/efs/tests/posix/posix_2client.py --local \
        '$XFS_DIR' '$XFS_DIR' --results /tmp/posix2-xfs.tsv; \
        cat /tmp/posix2-xfs.tsv" > "$pdir/xfs-baseline.tsv"
    say "  baseline: $(grep -c $'\tPASS' "$pdir/xfs-baseline.tsv") pass"

    say "posix2: efs A=$host_a B=$host_b mnt=$EFS_MNT"
    ensure_mounted "$host_a" || { say "  $host_a not mounted"; return 1; }
    ensure_mounted "$host_b" || { say "  $host_b not mounted"; return 1; }
    $SSH "$host_a" "python3 '$py' --prepare '$EFS_MNT'"
    # RPC metadata reads: B should see A's prepare without remount.
    # Remount only if the parent is missing (and never block forever on FUSE).
    if ! $SSH "$host_b" "timeout -k 2 15 test -d '$EFS_MNT/posix-2c'"; then
        say "  $host_b cannot see parent yet; remounting"
        remount_client "$host_b" || return 1
        $SSH "$host_b" "timeout -k 2 15 test -d '$EFS_MNT/posix-2c'" || {
            say "  $host_b still cannot see $EFS_MNT/posix-2c after remount"
            return 1
        }
    fi
    python3 "$py" --remote "$host_a" "$host_b" --mnt "$EFS_MNT" \
        --results "$pdir/efs-${host_a%.ib}-${host_b%.ib}.tsv"
    local rc=$?
    $SSH "$host_a" "rm -rf '$EFS_MNT/posix-2c'" || true

    say "  --- compare two-client efs vs XFS ---"
    python3 "$REPO/tests/posix/compare.py" "$pdir/xfs-baseline.tsv" \
        "$pdir/efs-${host_a%.ib}-${host_b%.ib}.tsv" | \
        tee "$pdir/compare-${host_a%.ib}-${host_b%.ib}.txt"
    [ ${PIPESTATUS[0]} -ne 0 ] && rc=1
    say "posix2 results in $pdir"
    return $rc
}

# ----------------------------------------------------------------- perf ---
perf_one() { # host mode outdir  (runs on the node, collects TSV back)
    local h=$1 mode=$2 outdir=$3
    push_tests "$h"
    $SSH "$h" "bash /tmp/efs/tests/perf/perf_node.sh '$EFS_MNT' \
        /tmp/perf-$RUN_ID.tsv '$mode' >/dev/null 2>&1; cat /tmp/perf-$RUN_ID.tsv"
}

cmd_perf() { # single|multi [host ...] [quick|full]
    local sub=$1; shift
    local hosts=() mode=full
    if [ "$sub" = single ]; then
        hosts=("${1:?perf single needs a host}")
        shift
        [ $# -gt 0 ] && mode=$1
    else # multi
        for a in "$@"; do
            case "$a" in quick|full) mode=$a ;; *) hosts+=("$a") ;; esac
        done
        [ ${#hosts[@]} -eq 0 ] && hosts=("${DEFAULT_HOSTS[@]}")
    fi
    local pdir="$RESULTS/perf/$RUN_ID"
    mkdir -p "$pdir"
    say "perf ($sub, $mode) on: ${hosts[*]}"

    # make sure every client is mounted before benchmarking
    for h in "${hosts[@]}"; do ensure_mounted "$h" || say "  WARN: $h not mounted"; done

    # run all hosts in parallel, one TSV each
    local pids=()
    for h in "${hosts[@]}"; do
        perf_one "$h" "$mode" "$pdir" > "$pdir/perf-${h%.ib}.tsv" 2>&1 &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p"; done

    # aggregate into a run summary + append to history
    local summary="$pdir/summary.tsv"
    {
        echo -e "run_id\thost\tsuite\ttest\tbw_mib_s\tiops\trc"
        for h in "${hosts[@]}"; do
            grep -vE '^(ts|===|PERF_NODE_DONE)' "$pdir/perf-${h%.ib}.tsv" 2>/dev/null | \
                awk -v rid="$RUN_ID" -F'\t' 'NF>=7{print rid"\t"$2"\t"$3"\t"$4"\t"$5"\t"$6"\t"$7}'
        done
    } > "$summary"
    # history: one line per (run, suite, test) summed across hosts
    awk -F'\t' 'NR>1 && NF>=7 { k=$3"\t"$4; bw[k]+=$5; n[k]++ }
        END { for (k in bw) printf "%s\t%s\t%.1f\t%d\n", "'"$RUN_ID"'", k, bw[k], n[k] }' \
        "$summary" >> "$RESULTS/perf/history.tsv"
    say "perf results in $pdir ; history appended"
    column -t -s$'\t' "$summary" | head -40
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
            $SSH "$h" "bash /tmp/efs/tests/perf/perf_local_nvme.sh \
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

# --------------------------------------------------------------- ewrite ---
# 30s ewrite.sh concurrency sweep (1 2 / 1 4 / 1 8 / 1 16) on one client.
cmd_ewrite() { # [host]
    local host=${1:-fcstor007.ib}
    local pdir="$RESULTS/ewrite/$RUN_ID"
    mkdir -p "$pdir" "$RESULTS/ewrite"
    say "ewrite: 30s sweep (1 2/4/8/16) on $host:$EFS_MNT"
    ensure_mounted "$host" || { say "  $host not mounted"; return 1; }
    push_tests "$host"
    $SSH "$host" "bash /tmp/efs/tests/perf/ewrite_sweep.sh '$EFS_MNT' \
        /tmp/ewrite-$RUN_ID.tsv; cat /tmp/ewrite-$RUN_ID.tsv" \
        | tee "$pdir/ewrite-${host%.ib}.tsv"
    # keep a clean TSV (drop the human lines the script prints)
    awk 'BEGIN{FS=OFS="\t"} $1=="ts" || $1 ~ /^[0-9]{4}-/' \
        "$pdir/ewrite-${host%.ib}.tsv" > "$pdir/summary.tsv"
    if [ -s "$RESULTS/ewrite/history.tsv" ]; then
        :
    else
        echo -e "run_id\thost\tjobs\twall_s\tbytes\tmib_s\trc" \
            > "$RESULTS/ewrite/history.tsv"
    fi
    awk -v rid="$RUN_ID" -F'\t' 'NR>1 && NF>=7 {
            print rid"\t"$2"\t"$3"\t"$4"\t"$5"\t"$6"\t"$7
        }' "$pdir/summary.tsv" >> "$RESULTS/ewrite/history.tsv"
    say "ewrite results in $pdir"
    echo "--- ewrite 30s sweep ---"
    awk -F'\t' 'NR>1 {
            printf "  jobs=%-3s  %.3fs  %.3f GiB  %8.1f MiB/s\n",
                $3, $4, $5/1073741824.0, $6
        }' "$pdir/summary.tsv"
}

# ----------------------------------------------------------------- meta ---
# efs-bench --meta on one client (workers 1/4/16). Build efs-bench on the node.
cmd_meta() { # [host]
    local host=${1:-fcstor007.ib}
    local pdir="$RESULTS/meta/$RUN_ID"
    mkdir -p "$pdir" "$RESULTS/meta"
    say "meta: efs-bench --meta on $host (workers 1/4/16)"
    $SSH "$host" 'rsync -a --delete --exclude="/mnt/" --exclude="*.log" \
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
        $SSH "$host" "cd /tmp/efs && ./efs-bench 172.16.223.57:19810 --meta \
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

# ------------------------------------------------------------------ all ---
cmd_all() { # [efs-host ...]
    local hosts=("$@")
    [ ${#hosts[@]} -eq 0 ] && hosts=("${DEFAULT_HOSTS[@]}")
    cmd_posix "${hosts[@]}"
    cmd_perf multi "${hosts[@]}"
}

main() {
    mkdir -p "$RESULTS/posix" "$RESULTS/posix2" "$RESULTS/perf" "$RESULTS/nvme" \
             "$RESULTS/ewrite" "$RESULTS/meta"
    touch "$RESULTS/perf/history.tsv"
    local cmd=${1:-}
    shift || true
    case "$cmd" in
        posix) cmd_posix "$@" ;;
        posix2) cmd_posix2 "$@" ;;
        perf)  cmd_perf "$@" ;;
        nvme)  cmd_nvme "$@" ;;
        ewrite) cmd_ewrite "$@" ;;
        meta) cmd_meta "$@" ;;
        setup) cmd_setup "$@" ;;
        all)   cmd_all "$@" ;;
        *) sed -n '2,21p' "$0"; return 2 ;;
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
