#!/bin/bash
# Same-user observational profile. Invoke separately on each host, specifying
# only the Intel deployment's PIDs (not other clusters on the same machine).
# Usage: bash profile_intel_live.sh NEW_OUTPUT_DIR PID[,PID...] [PERF_SECONDS]
set -euo pipefail
out=${1:?new output directory}; pids=${2:?comma separated PIDs}; seconds=${3:-45}
[[ $pids =~ ^[0-9]+(,[0-9]+)*$ && $seconds =~ ^[0-9]+$ ]]
(( seconds >= 1 && seconds <= 120 ))
test ! -e "$out"
IFS=, read -ra targets <<< "$pids"
for pid in "${targets[@]}"; do
    test "$(stat -c %u /proc/"$pid")" = "$(id -u)"
done
mkdir -p "$out"
snapshot() {
    date -u
    uptime
    cat /proc/net/snmp /proc/net/netstat /proc/net/dev /proc/diskstats
    ss -tin
    for pid in "${targets[@]}"; do
        echo "PID $pid"
        sha256sum "/proc/$pid/exe"
        tr '\0' ' ' < "/proc/$pid/cmdline"; echo
        cat "/proc/$pid/io" "/proc/$pid/stat" "/proc/$pid/status"
    done
}
snapshot > "$out/before.txt"
perf record -e cycles:u -F 99 --call-graph fp -p "$pids" \
    -o "$out/perf.data" -- sleep "$seconds" > "$out/perf-record.txt" 2>&1 &
perf_job=$!
python3 - "$pids" "$seconds" > "$out/waits.jsonl" <<'PY'
import collections,json,pathlib,sys,time
for i in range(int(sys.argv[2])):
    row={'time':time.time(),'pids':{}}
    for pid in sys.argv[1].split(','):
        p=pathlib.Path('/proc')/pid
        waits=collections.Counter(); states=collections.Counter()
        for t in (p/'task').iterdir():
            try:
                waits[(t/'wchan').read_text().strip()]+=1
                states[(t/'stat').read_text().rsplit(')',1)[1].split()[0]]+=1
            except FileNotFoundError: pass
        row['pids'][pid]={'waits':dict(waits),'states':dict(states)}
    print(json.dumps(row),flush=True);time.sleep(1)
PY
wait "$perf_job"
snapshot > "$out/after-perf.txt"
perf report -i "$out/perf.data" --stdio --no-children --call-graph none \
    --sort pid,comm,dso,symbol --percent-limit 0.5 > "$out/perf-flat.txt"
perf report -i "$out/perf.data" --stdio --no-children --call-graph none \
    --sort comm,dso,symbol --percent-limit 1 > "$out/perf-functions.txt"
perf report -i "$out/perf.data" --stdio --children --percent-limit 2 \
    > "$out/perf-callers.txt"
# Wall-clock syscall duration, including sleep/wait, sums across threads and
# therefore is not a fraction of elapsed workload time. No payloads recorded.
jobs=()
for pid in "${targets[@]}"; do
    timeout -s INT 12 strace -f -c -w -p "$pid" \
        -o "$out/strace-$pid.txt" > "$out/strace-$pid.attach" 2>&1 &
    jobs+=("$!")
done
for job in "${jobs[@]}"; do wait "$job" || test "$?" = 124; done
snapshot > "$out/after-strace.txt"
echo "Saved $out"
