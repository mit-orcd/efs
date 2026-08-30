#!/bin/bash
# Honest fio matrix: the 9 suite jobs at 1 / 4 / 9 clients, plus a 50g file.
# Writes use --end_fsync=1 (no time_based). Reads run after a remount so
# they cannot be served from the local efs dcache.
set -u
SSH="${EFS_SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
MNT=/tmp/efs-mount
OUT="${1:-$HOME/git/efs/results/perf/$(date +%Y%m%d-%H%M%S)-honest}"
JOBS=${FIO_JOBS:-9}
SIZE=${FIO_SIZE:-2g}
SIZE4K=${FIO_SIZE4K:-256m}
BIG50=${FIO_50G:-50g}
mkdir -p "$OUT"

ssh_to() { # timeout host cmd
    local t=$1 h=$2; shift 2
    EFS_SSH_TIMEOUT=$t "$SSH" "${h}.ib" "$@"
}

H1=(fcstor007)
H4=(fcstor007 fcstor008 fcstor009 fcstor010)
H9=(fcstor007 fcstor008 fcstor009 fcstor010 fcstor011 fcstor012 fcstor013 fcstor014 fcstor015)

remount() { # hosts...
    local h pids=()
    for h in "$@"; do
        ssh_to 25 "$h" "killall -9 efs-fuse 2>/dev/null || true
            timeout 3 fusermount3 -uz $MNT 2>/dev/null || true
            cd /tmp/efs && mkdir -p $MNT && rm -f fuse.log
            EFS_TRANSPORT="${EFS_TRANSPORT:-}" setsid ./efs-fuse 172.16.223.57:19810 efs-test $MNT >fuse.log 2>&1 </dev/null &
            for i in \$(seq 1 40); do
                grep -q \"efs-fuse $MNT \" /proc/mounts && exit 0
                sleep 0.15
            done
            echo remount-fail; tail -5 fuse.log; exit 1" >/dev/null &
        pids+=($!)
    done
    local rc=0 p
    for p in "${pids[@]}"; do wait "$p" || rc=1; done
    return $rc
}

# stdout: MiB/s or FAIL
parse_bw() {
    local f=$1 kind=$2
    local line unit num
    line=$(grep -E "^  ${kind}: IOPS=" "$f" | head -1)
    [ -n "$line" ] || { echo FAIL; return; }
    num=$(echo "$line" | sed -nE 's/.*BW=([0-9.]+)([KMG])iB\/s.*/\1/p')
    unit=$(echo "$line" | sed -nE 's/.*BW=([0-9.]+)([KMG])iB\/s.*/\2/p')
    case "$unit" in
        K) awk -v n="$num" 'BEGIN{printf "%.1f", n/1024}' ;;
        M) awk -v n="$num" 'BEGIN{printf "%.1f", n}' ;;
        G) awk -v n="$num" 'BEGIN{printf "%.1f", n*1024}' ;;
        *) echo FAIL ;;
    esac
}

# Run one fio job on all hosts in parallel. Writes get --end_fsync=1.
# slot is the per-host directory (big | small) so reads hit the written files.
run_job() { # name slot rw bs size kind hosts...
    local name=$1 slot=$2 rw=$3 bs=$4 size=$5 kind=$6; shift 6
    local hosts=("$@") extra="" tmo=400
    [ "$kind" = write ] && extra="--end_fsync=1"
    local h pids=()
    for h in "${hosts[@]}"; do
        local dir="$MNT/fio-h/$h/$slot"
        ssh_to "$tmo" "$h" "
            fst=\$(findmnt -n -o FSTYPE '$MNT' 2>/dev/null || true)
            echo FUSE_CHECK fstype=\$fst mnt=$MNT
            echo \$fst | grep -q '^fuse\\.efs-fuse\$' || { echo NOT_FUSE; exit 1; }
            echo FUSE_OK
            mkdir -p '$dir' && fio --name=$name --directory='$dir' \
            --filename_format='f.\$jobnum' --rw=$rw --bs=$bs --size=$size \
            --numjobs=$JOBS --ioengine=psync --direct=1 --group_reporting \
            $extra 2>&1" >"$OUT/${#hosts[@]}-${name}-${h}.txt" &
        pids+=($!)
    done
    local p
    for p in "${pids[@]}"; do wait "$p"; done
    local tot=0 bw n=0
    for h in "${hosts[@]}"; do
        if grep -q NOT_FUSE "$OUT/${#hosts[@]}-${name}-${h}.txt" 2>/dev/null || \
           grep -qE 'md0| /dev/sd' "$OUT/${#hosts[@]}-${name}-${h}.txt" 2>/dev/null; then
            echo "ABORT $h $name: not the FUSE export (NOT_FUSE or local-disk stats)"
            bw=FAIL
        else
            bw=$(parse_bw "$OUT/${#hosts[@]}-${name}-${h}.txt" "$kind")
        fi
        echo -e "${name}\t${#hosts[@]}\t${h}\t${bw}" | tee -a "$OUT/raw.tsv"
        if [ "$bw" != FAIL ]; then
            tot=$(awk -v a="$tot" -v b="$bw" 'BEGIN{printf "%.1f", a+b}')
            n=$((n+1))
        fi
    done
    echo -e "${name}\t${#hosts[@]}\tAGG\t${tot}" | tee -a "$OUT/raw.tsv"
}

run_50g() { # hosts...
    local hosts=("$@") name=sw-50g
    local h pids=()
    for h in "${hosts[@]}"; do
        local dir="$MNT/fio-h/$h/$name"
        ssh_to 600 "$h" "
            fst=\$(findmnt -n -o FSTYPE '$MNT' 2>/dev/null || true)
            echo FUSE_CHECK fstype=\$fst mnt=$MNT
            echo \$fst | grep -q '^fuse\\.efs-fuse\$' || { echo NOT_FUSE; exit 1; }
            echo FUSE_OK
            mkdir -p '$dir' && fio --name=$name --directory='$dir' \
            --filename=big50 --rw=write --bs=1m --size=$BIG50 --numjobs=1 \
            --ioengine=psync --direct=1 --end_fsync=1 --group_reporting 2>&1" \
            >"$OUT/${#hosts[@]}-${name}-${h}.txt" &
        pids+=($!)
    done
    local p
    for p in "${pids[@]}"; do wait "$p"; done
    local tot=0 bw
    for h in "${hosts[@]}"; do
        if grep -q NOT_FUSE "$OUT/${#hosts[@]}-${name}-${h}.txt" 2>/dev/null || \
           grep -qE 'md0| /dev/sd' "$OUT/${#hosts[@]}-${name}-${h}.txt" 2>/dev/null; then
            echo "ABORT $h $name: not the FUSE export"; bw=FAIL
        else
            bw=$(parse_bw "$OUT/${#hosts[@]}-${name}-${h}.txt" write)
        fi
        echo -e "${name}\t${#hosts[@]}\t${h}\t${bw}" | tee -a "$OUT/raw.tsv"
        [ "$bw" != FAIL ] && tot=$(awk -v a="$tot" -v b="$bw" 'BEGIN{printf "%.1f", a+b}')
    done
    echo -e "${name}\t${#hosts[@]}\tAGG\t${tot}" | tee -a "$OUT/raw.tsv"
    remount "${hosts[@]}"
    pids=()
    name=sr-50g
    for h in "${hosts[@]}"; do
        local dir="$MNT/fio-h/$h/sw-50g"
        ssh_to 600 "$h" "
            fst=\$(findmnt -n -o FSTYPE '$MNT' 2>/dev/null || true)
            echo FUSE_CHECK fstype=\$fst mnt=$MNT
            echo \$fst | grep -q '^fuse\\.efs-fuse\$' || { echo NOT_FUSE; exit 1; }
            echo FUSE_OK
            fio --name=$name --directory='$dir' --filename=big50 \
            --rw=read --bs=1m --size=$BIG50 --numjobs=1 --ioengine=psync \
            --direct=1 --group_reporting 2>&1" \
            >"$OUT/${#hosts[@]}-${name}-${h}.txt" &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p"; done
    tot=0
    for h in "${hosts[@]}"; do
        bw=$(parse_bw "$OUT/${#hosts[@]}-${name}-${h}.txt" read)
        echo -e "${name}\t${#hosts[@]}\t${h}\t${bw}" | tee -a "$OUT/raw.tsv"
        [ "$bw" != FAIL ] && tot=$(awk -v a="$tot" -v b="$bw" 'BEGIN{printf "%.1f", a+b}')
    done
    echo -e "${name}\t${#hosts[@]}\tAGG\t${tot}" | tee -a "$OUT/raw.tsv"
}

sweep() { # label hosts...
    local label=$1; shift
    local hosts=("$@")
    echo "=== $label clients: ${hosts[*]} ==="
    remount "${hosts[@]}" || { echo "remount failed for $label"; return 1; }
    # same files as the suite: 9x SIZE in big/, 9x SIZE4K in small/
    run_job sw-1m   big   write     1m   "$SIZE"   write "${hosts[@]}"
    run_job ow-1m   big   write     1m   "$SIZE"   write "${hosts[@]}"
    run_job rw-1m   big   randwrite 1m   "$SIZE"   write "${hosts[@]}"
    run_job rw-128k big   randwrite 128k "$SIZE"   write "${hosts[@]}"
    run_job rw-4k   small randwrite 4k   "$SIZE4K" write "${hosts[@]}"
    remount "${hosts[@]}" || { echo "remount-before-read failed"; return 1; }
    run_job sr-1m   big   read      1m   "$SIZE"   read  "${hosts[@]}"
    run_job rr-1m   big   randread  1m   "$SIZE"   read  "${hosts[@]}"
    run_job rr-128k big   randread  128k "$SIZE"   read  "${hosts[@]}"
    run_job rr-4k   small randread  4k   "$SIZE4K" read  "${hosts[@]}"
    echo "=== $label clients 50g ==="
    run_50g "${hosts[@]}"
}

echo -e "test\tclients\thost\tbw_mib_s" >"$OUT/raw.tsv"
echo "OUT=$OUT jobs=$JOBS size=$SIZE size4k=$SIZE4K"
sweep 1 "${H1[@]}"
sweep 4 "${H4[@]}"
sweep 9 "${H9[@]}"
echo DONE "$OUT"
