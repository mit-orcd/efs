#!/bin/bash
# K99 reproduction: repeat the k96_status_slots_live TSan entry and count
# frag_loc_note reports per run.
#   usage: k99_tsan_reps.sh TREE OUT REPS
# TREE is a source tree root. The first call builds a TSan tree under
# OUT/build0 (one full-list-free run); every rep reuses that build.
# Writes OUT/reps.tsv: rep, gate rc, ThreadSanitizer warnings,
# frag_loc_note reports, host_export_salt reports (K98, not this row).
set -u
TREE=$1 OUT=$2 REPS=$3
mkdir -p "$OUT" || exit 2
cd "$TREE" || exit 2
count() { cat "$1"/*.tsan.* 2>/dev/null | grep -c "$2"; }
# Reports are separated by WARNING lines; count reports naming the frame.
reports_with() {
    cat "$1"/*.tsan.* 2>/dev/null |
        awk -v pat="$2" '/^WARNING: ThreadSanitizer:/{if(hit)n++; hit=0} $0~pat{hit=1} END{if(hit)n++; print n+0}'
}
if [ ! -x "$OUT/build0/build/efsd" ]; then
    EFS_SAN_LOGDIR="$OUT/build0" EFS_SAN_ONLY=k96_status_slots_live \
        bash tests/sanitizer_check.sh tsan >"$OUT/build0.log" 2>&1
    echo "build0 rc=$?" >>"$OUT/build0.log"
fi
[ -f "$OUT/reps.tsv" ] || printf 'rep\trc\twarnings\tfrag_loc_note\thost_export_salt\n' >"$OUT/reps.tsv"
for i in $(seq 1 "$REPS"); do
    r=$(printf 'r%03d' "$i")
    [ -d "$OUT/$r" ] && continue
    EFS_SAN_BUILD="$OUT/build0/build" EFS_SAN_LOGDIR="$OUT/$r" \
        EFS_SAN_ONLY=k96_status_slots_live \
        bash tests/sanitizer_check.sh tsan >"$OUT/$r.log" 2>&1
    rc=$?
    printf '%s\t%s\t%s\t%s\t%s\n' "$r" "$rc" \
        "$(count "$OUT/$r" '^WARNING: ThreadSanitizer:')" \
        "$(reports_with "$OUT/$r" frag_loc)" \
        "$(reports_with "$OUT/$r" host_export_salt)" >>"$OUT/reps.tsv"
done
echo done >"$OUT/DONE"
