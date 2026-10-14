#!/usr/bin/env bash
# Native Linux, same-user live sampling. No ptrace attach, sudo or restart.
# Usage: scripts/profile-live-efsd.sh PID OUTPUT_DIRECTORY [SECONDS]
set -euo pipefail
pid=${1:?efsd PID required}
out=${2:?new output directory required}
seconds=${3:-45}
[[ "$pid" =~ ^[0-9]+$ && "$seconds" =~ ^[0-9]+$ ]]
(( seconds >= 1 && seconds <= 600 ))
test "$(stat -c %u /proc/$pid)" = "$(id -u)"
test "$(cat /proc/$pid/comm)" = efsd
test ! -e "$out"
mkdir -p "$out"
sha256sum "/proc/$pid/exe" > "$out/binary-sha256"
tr '\0' '\n' < "/proc/$pid/cmdline" > "$out/arguments"
cat "/proc/$pid/io" > "$out/io-before"
pidstat -t -u -r -d -w -p "$pid" 1 "$seconds" > "$out/pidstat.txt" 2>&1 &
stat_job=$!
perf record -e cycles:u -F 99 --call-graph fp -p "$pid" -o "$out/perf.data" -- sleep "$seconds" > "$out/perf-record.txt" 2>&1 &
perf_job=$!
python3 - "$pid" "$out" "$seconds" <<'PY'
import sys,pathlib,time,json,collections
p=pathlib.Path('/proc')/sys.argv[1]
with (pathlib.Path(sys.argv[2])/'waits.jsonl').open('w') as f:
 for i in range(int(sys.argv[3])):
  waits=collections.Counter();states=collections.Counter()
  for t in (p/'task').iterdir():
   try:
    waits[(t/'wchan').read_text().strip()]+=1
    states[(t/'stat').read_text().rsplit(')',1)[1].split()[0]]+=1
   except FileNotFoundError:pass
  f.write(json.dumps(dict(sample=i,time=time.time(),waits=dict(waits),states=dict(states)))+'\n')
  f.flush();time.sleep(1)
PY
wait "$stat_job"
if wait "$perf_job"; then
 perf report -i "$out/perf.data" --stdio --no-children --call-graph none --percent-limit 0.5 > "$out/perf-flat.txt"
 perf report -i "$out/perf.data" --stdio --children --percent-limit 2 > "$out/perf-callers.txt"
else
 echo 'perf failed; retained pidstat and /proc observations; see perf-record.txt' >&2
 exit 1
fi
cat "/proc/$pid/io" > "$out/io-after"
echo "Profile saved: $out"
