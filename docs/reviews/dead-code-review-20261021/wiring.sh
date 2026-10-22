#!/bin/bash
# wiring.sh — run from the repository root. Lists tests, binaries and env
# flags whose wiring the dead-code review checked. Read-only; no build.
set -u
echo "== tests/*.py never named in the Makefile"
for f in tests/*.py; do grep -q "$f" Makefile || echo "$f"; done
echo "== tests/*.c never named in the Makefile"
for f in tests/*.c; do b=${f%.c}; grep -qE "$b([^_a-z]|\$)" Makefile || echo "$f"; done
echo "== TEST_BINS built by 'all' but never executed by a Makefile recipe"
for b in $(make -pn 2>/dev/null | sed -n 's/^TEST_BINS = //p' | head -1); do
    grep -qE "^\s.*\./$b( |\$)" Makefile || echo "$b"
done
echo "== Makefile python tests reachable only from targets 'test' does not run"
awk '/^test-client-memory:/{f=1;next} /^[^\t]/{f=0} f' Makefile |
    grep -o 'tests/[a-z_]*\.py' | while read -r t; do
        [ "$(grep -c "$t" Makefile)" -lt 2 ] && echo "$t"
    done
echo "== EFS_* tokens in src with no setter in tests/, scripts/, tools/"
for e in $(git grep -h -o -E '"EFS_[A-Z0-9_]+"' -- src include | tr -d '"' | sort -u); do
    git grep -q -w "$e" -- tests scripts tools efs-bench.sh Makefile || echo "$e"
done
echo "== EFS_* tokens in user docs that no code mentions"
git grep -h -o -E 'EFS_[A-Z0-9_]+[A-Z0-9]' -- docs/using docs/operations | sort -u |
    while read -r e; do git grep -q -w "$e" -- src include tests scripts || echo "$e"; done
echo "== request messages handled by efsd with no sender outside handler.c"
for m in $(grep -o -E 'EFS_MSG_[A-Z0-9_]+ *= *[0-9]+' include/efs/protocol.h | awk '{print $1}' | grep -v -E '_(REPLY|ACK)$'); do
    git grep -n -w "$m" -- src ':!src/sim' ':!include' ':!src/server/handler.c' | grep -q . || echo "$m"
done
echo "== metadata commands never proposed outside the apply path"
for c in $(grep -o -E '^#define EFS_MD_CMD_[A-Z0-9_]+ +[0-9]+' include/efs/meta_cmd.h | awk '{print $2}'); do
    git grep -n -w "$c" -- src ':!src/sim' ':!include' |
        grep -v -E "cmd\[0\] *== *$c|case +$c" | grep -q . || echo "$c"
done
