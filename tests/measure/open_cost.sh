#!/bin/bash
# open_cost.sh — W6 residual 2: opening a 1 GiB file costs ~20 s (128
# sequential GETCHUNKS + a 64-lane stat per open); is it linear in size and
# what RPCs make it up?
#
# For each size in SIZES (MiB): write the file on WRITER with dd+fsync
# (non-zero source), cold-remount READER with EFS_RPC_PROF=1 (same binary),
# then on READER time open() / first 4 KiB read() / close, twice (2nd is the
# warm number), and read the client's RPC-PROF counters (getchunks, lookup,
# getattr count and server+wire wait). Files stay under $MNT/measure/ so a
# later run can skip the write (SKIP_WRITE=1).
#
# Usage (from node9901 via efs-bg.sh; ~3 min for the default sizes):
#   bash tests/measure/open_cost.sh
#   SIZES="128 1024 4096" SKIP_WRITE=1 bash tests/measure/open_cost.sh
#
# Hand the user: cold open ms vs size (linear = per-chunk fetch; 1 GiB =
# 8192 chunks / 64 per GETCHUNKS = 128 round trips), getchunks_n and its
# avg recv_us, and whether the WARM open is cheap (it should be — if not,
# the client is not keeping the map). The spec answer is §8 per-lane range
# fetch (performance.md) — not implemented; do not implement it unasked.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/lib.sh"
SIZES="${SIZES:-128 1024 4096}"
WRITER="${WRITER:-fcstor007}"
READER="${READER:-fcstor008}"
SKIP_WRITE="${SKIP_WRITE:-0}"
mkout open-cost
preflight_or_die

printf 'size_mib\tcold_open_ms\tcold_read4k_ms\tcold_total_ms\twarm_open_ms\twarm_read4k_ms\tgetchunks_n\tgetchunks_avg_us\tlookup_n\tgetattr_n\tcalls\n' > "$OUT/table.tsv"
ssh_ 10 "$WRITER" "mkdir -p $MNT/measure" >/dev/null
[ "$SKIP_WRITE" = 1 ] || ensure_src "$WRITER" >/dev/null

for mb in $SIZES; do
    f="$MNT/measure/open-${mb}m.bin"
    if [ "$SKIP_WRITE" != 1 ]; then
        say "== write $mb MiB on $WRITER (dd+fsync)"
        ssh_ 400 "$WRITER" "TIMEFORMAT='WRITE_WALL %R'; { time dd if=/tmp/src8g of=$f bs=1M count=$mb conv=fsync status=none; } 2>&1; ls -l $f" | tee -a "$OUT/write-$mb.txt"
    fi
    say "== cold remount $READER with EFS_RPC_PROF=1"
    remount_clients "$READER" "EFS_RPC_PROF=1" > "$OUT/remount-$mb.txt" || { cat "$OUT/remount-$mb.txt"; exit 1; }
    say "== open/read on $READER"
    ssh_ 120 "$READER" "python3 - '$f' <<'PY'
import os, sys, time
p = sys.argv[1]
for tag in ('cold', 'warm'):
    t0 = time.monotonic(); fd = os.open(p, os.O_RDONLY); t1 = time.monotonic()
    os.read(fd, 4096); t2 = time.monotonic()
    os.fstat(fd); os.close(fd)
    print('%s open_ms=%.1f read4k_ms=%.1f total_ms=%.1f' % (tag, (t1-t0)*1e3, (t2-t1)*1e3, (t2-t0)*1e3), flush=True)
PY" > "$OUT/open-$mb.txt"
    cat "$OUT/open-$mb.txt"
    line=$(rpc_prof_last "$READER")   # waits out the 2 s dump interval itself; echo "$line" > "$OUT/rpcprof-$mb.txt"
    co=$(grep '^cold' "$OUT/open-$mb.txt" | grep -o 'open_ms=[0-9.]*' | cut -d= -f2)
    cr=$(grep '^cold' "$OUT/open-$mb.txt" | grep -o 'read4k_ms=[0-9.]*' | cut -d= -f2)
    ct=$(grep '^cold' "$OUT/open-$mb.txt" | grep -o 'total_ms=[0-9.]*' | cut -d= -f2)
    wo=$(grep '^warm' "$OUT/open-$mb.txt" | grep -o 'open_ms=[0-9.]*' | cut -d= -f2)
    wr=$(grep '^warm' "$OUT/open-$mb.txt" | grep -o 'read4k_ms=[0-9.]*' | cut -d= -f2)
    gn=$(n0 "$(rpc_field_n "$line" getchunks)"); gus=$(n0 "$(rpc_field_us "$line" getchunks)")
    gavg=$([ "$gn" -gt 0 ] && echo $(( gus / gn )) || echo 0)
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$mb" "$co" "$cr" "$ct" "$wo" "$wr" "$gn" "$gavg" \
        "$(n0 "$(rpc_field_n "$line" lookup)")" "$(n0 "$(rpc_field_n "$line" getattr)")" "$(n0 "$(rpc_field "$line" calls)")" >> "$OUT/table.tsv"
done

# Parallel phase: IOR easy-read has 36 ranks opening 1 GiB files at once.
# PAR="hosts x procs" cold-opens the largest file from that many procs
# simultaneously and reports the open-time distribution — the number that
# maps to the 20 s seen in IOR. PAR="" skips it.
PAR="${PAR:-8x4}"
if [ -n "$PAR" ]; then
    ph=${PAR%x*}; pp=${PAR#*x}; big=${SIZES##* }; f="$MNT/measure/open-${big}m.bin"
    phosts=$(echo $ALL_CLIENTS | tr ' ' '\n' | grep -v "^$WRITER$" | head -"$ph" | tr '\n' ' ')
    say "== parallel: $ph hosts x $pp procs cold-open $f at once"
    remount_clients "$phosts" "EFS_RPC_PROF=1" > "$OUT/remount-par.txt" || { cat "$OUT/remount-par.txt"; exit 1; }
    for h in $phosts; do
        ssh_ 300 "$h" "python3 - '$f' $pp <<'PY'
import os, sys, time
p, n = sys.argv[1], int(sys.argv[2])
kids = []
for i in range(n):
    pid = os.fork()
    if pid == 0:
        t0 = time.monotonic(); fd = os.open(p, os.O_RDONLY); t1 = time.monotonic()
        os.read(fd, 4096); t2 = time.monotonic(); os.close(fd)
        print('par open_ms=%.1f read4k_ms=%.1f' % ((t1-t0)*1e3, (t2-t1)*1e3), flush=True)
        os._exit(0)
    kids.append(pid)
for k in kids: os.waitpid(k, 0)
PY" > "$OUT/par-$h.txt" 2>&1 &
    done
    wait
    cat "$OUT"/par-*.txt | grep -o 'open_ms=[0-9.]*' | cut -d= -f2 | sort -n > "$OUT/par-open-ms.txt"
    np_=$(wc -l < "$OUT/par-open-ms.txt")
    pmin=$(head -1 "$OUT/par-open-ms.txt"); pmax=$(tail -1 "$OUT/par-open-ms.txt")
    pmed=$(sed -n "$(( (np_ + 1) / 2 ))p" "$OUT/par-open-ms.txt")
    gtot=0; gus=0
    for h in $phosts; do line=$(rpc_prof_last "$h"); echo "$h $line" >> "$OUT/rpcprof-par.txt"
        gtot=$(( gtot + $(n0 "$(rpc_field_n "$line" getchunks)") )); gus=$(( gus + $(n0 "$(rpc_field_us "$line" getchunks)") )); done
    gavg=$([ "$gtot" -gt 0 ] && echo $(( gus / gtot )) || echo 0)
    PAR_LINE="parallel cold open of the ${big} MiB file from $np_ procs ($PAR): open_ms min/med/max = $pmin / $pmed / $pmax; getchunks_n=$gtot avg_us=$gavg (vs single-opener avg above)"
    say "$PAR_LINE"
    remount_clients "$phosts" "" > "$OUT/remount-par-restore.txt" || cat "$OUT/remount-par-restore.txt"
fi

say "restore $READER (plain remount)"
remount_clients "$READER" "" > "$OUT/remount-restore.txt" || cat "$OUT/remount-restore.txt"
{
    echo "open cost vs file size, build=$BUILD, $(date -u +%F), writer=$WRITER reader=$READER (cold remount before each size)"
    column -t -s $'\t' "$OUT/table.tsv"
    echo
    echo "chunks = size_mib*8; GETCHUNKS returns <=64 records, so expected getchunks_n ~ chunks/64 per open if the whole map is fetched at open."
    [ -z "${PAR_LINE:-}" ] || { echo; echo "$PAR_LINE"; echo "(getchunks_avg_us growing with openers = the server serializes/queues GETCHUNKS; flat = client-side)"; }
    echo "Raw: open-*.txt rpcprof-*.txt write-*.txt par-*.txt"
} > "$OUT/SUMMARY.txt"
cat "$OUT/SUMMARY.txt"
