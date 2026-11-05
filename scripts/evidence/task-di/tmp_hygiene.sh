#!/bin/bash
# Task DI /tmp hygiene on an ORCD host (docs/status/2026-11/tmp-hygiene-20261105.md).
# Usage: ssh <host> 'bash -s list' < tmp_hygiene.sh   (then remove)
# Only erbmi1-owned entries directly under /tmp, older than 24 h, in the EFS
# scratch families; listening endpoints and records of live pids are kept.
# Listings and logs go to ~/efs-artifacts/taskDI-20261105/<host>-<mode>-*.
set -u
MODE=$1
H=$(hostname -s)
OUT=$HOME/efs-artifacts/taskDI-20261105
mkdir -p "$OUT"
P="$OUT/$H-$MODE"
[ "$(whoami)" = erbmi1 ] || exit 2
FAMILIES=(-name 'efs-control-*.stop*' -o -name 'efs-control-*.sock' -o -name 'efs-gc-*' -o -name 'efs-sort-*'
  -o -name 'efs-query-*' -o -name 'efs-k41-*' -o -name 'efs-meta-reap-cleanup-*' -o -name 'efs-ba-upload-*'
  -o -name 'efs-subtree-*' -o -name 'efs-session-admission-cold-*' -o -name 'efs-retained-audit-*')
count() { # per-family counts of every erbmi1 efs-* entry in /tmp (any age)
  find /tmp -maxdepth 1 -user erbmi1 -name 'efs-*' -printf '%f\n' |
    sed -E 's/^efs-control-[0-9a-f]{16}\./efs-control-H./; s/-[A-Za-z0-9_]{6,8}$/-XXXXXX/; s/-[0-9]+$/-PID/' |
    sort | uniq -c | sort -rn
}
count > "$P-counts-before.txt"
live_socks=$(awk 'NR>1 && $8 ~ /^\/tmp\/efs-control-/ {print $8}' /proc/net/unix | sort -u)
: > "$P-candidates.tsv"; : > "$P-kept.tsv"
while IFS= read -r -d '' f; do
  keep=
  case $f in
    *.sock) grep -qxF "$f" <<<"$live_socks" && keep="listening socket" ;;
    *.stop*) pid=$(sed -n '1s/^pid=\([0-9]*\) .*/\1/p' "$f" 2>/dev/null)
             [ -n "$pid" ] && [ -d /proc/$pid ] && keep="record pid $pid alive ($(cat /proc/$pid/comm 2>/dev/null))" ;;
  esac
  line=$(stat -c '%F	%y	%s' "$f")
  if [ -n "$keep" ]; then printf '%s\t%s\t%s\n' "$f" "$line" "$keep" >> "$P-kept.tsv"
  else printf '%s\t%s\n' "$f" "$line" >> "$P-candidates.tsv"; fi
done < <(find /tmp -maxdepth 1 -mindepth 1 -user erbmi1 -mmin +1440 \( "${FAMILIES[@]}" \) -print0)
echo "candidates $(wc -l < "$P-candidates.tsv") kept $(wc -l < "$P-kept.tsv")"
cut -f2 "$P-candidates.tsv" | sort | uniq -c
if [ "$MODE" = remove ]; then
  cut -f1 "$P-candidates.tsv" > "$P-paths.txt"
  # never anything outside /tmp/efs-<family>, never task/merge/mount trees
  grep -vE '^/tmp/efs-(control-[0-9a-f]{16}\.(sock|stop.*)|(gc|sort|query|k41|meta-reap-cleanup|ba-upload|subtree|session-admission-cold|retained-audit)-[^/]+)$' "$P-paths.txt" && { echo "REFUSED: unexpected path"; exit 3; }
  ED=$HOME/bin/edelete
  while IFS= read -r f; do
    if [ -d "$f" ] && [ ! -L "$f" ]; then
      if [ -x "$ED" ]; then "$ED" --force --delete "$f" >/dev/null 2>&1 || rm -rf -- "$f"; else rm -rf -- "$f"; fi
    else rm -f -- "$f"; fi
    [ -e "$f" ] && echo "LEFT $f"
  done < "$P-paths.txt" > "$P-removal.log" 2>&1
  echo "left $(grep -c ^LEFT "$P-removal.log")"
  count > "$P-counts-after.txt"
fi
