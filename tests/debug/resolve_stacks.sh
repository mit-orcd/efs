#!/bin/bash
# Resolve an efs-fuse SIGUSR1 stack dump (see efs_fuse_stack_broadcast) into
# symbols. ptrace is admin-only on the test nodes, so this is how a wedged
# mount gets read. Usage: resolve_stacks.sh <stacks-file> [tid ...]
set -u
f=${1:?usage: resolve_stacks.sh <stacks-file> [tid ...]}
shift
bin=${EFS_FUSE_BIN:-/tmp/efs/efs-fuse}

resolve() { grep -oP '(?<=efs-fuse\[)0x[0-9a-f]+' | xargs -r addr2line -f -i -C -e "$bin"; }

if [ $# -gt 0 ]; then
    for t in "$@"; do
        echo "=== tid $t ==="
        awk -v t="$t" '$0 ~ ("STACKDUMP tid=" t " ") {p=1; next} /^STACKDUMP tid=/{p=0} p' "$f" | resolve
    done
    exit 0
fi

# No tids given: one entry per distinct stack, most common first.
python3 - "$f" <<'EOF' | while IFS= read -r line; do
import collections, sys
stacks = collections.Counter()
tids = {}
cur, frames = None, []
for line in open(sys.argv[1]):
    if line.startswith("STACKDUMP tid="):
        if cur is not None:
            stacks[tuple(frames)] += 1
            tids.setdefault(tuple(frames), cur)
        cur = line.split("tid=")[1].split()[0]
        frames = []
    elif line.startswith("./efs-fuse[") or line.startswith("/lib"):
        frames.append(line.strip())
if cur is not None:
    stacks[tuple(frames)] += 1
    tids.setdefault(tuple(frames), cur)
for st, n in stacks.most_common():
    print("#threads=%d tid=%s" % (n, tids[st]))
    for fr in st:
        print(fr)
EOF
    case "$line" in
        \#threads=*) echo; echo "=== $line ===" ;;
        *efs-fuse\[*) printf '%s\n' "$line" | resolve ;;
    esac
done
