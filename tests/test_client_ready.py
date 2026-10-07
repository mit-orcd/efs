#!/usr/bin/env python3
"""Execute production mount serving probes with deterministic timeout faults."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
s=(root/'scripts/client.sh').read_text()
a=s.index('client_wait_serving() {')
f=s[a:s.index('\n}',a)+2]
for succeed, mounted, expected, probes, waits in [
    (1, 1, 0, 1, 0), (2, 1, 0, 2, 1), (10, 1, 0, 10, 9),
    (0, 1, 1, 10, 9), (0, 0, 1, 0, 0), (1, 0, 1, 0, 0), (1, 2, 1, 1, 0),
]:
    script=f"calls=0; waits=0; mount_calls=0; succeed={succeed}; mounted={mounted}\n"+r'''
timeout() {
    [ "$1" = 3 ] && [ "$2" = stat ] && [ "$3" = '/test mount' ] || exit 91
    calls=$((calls+1)); [ "$calls" = "$succeed" ]
}
mountpoint() {
 [ "$1" = -q ] && [ "$2" = '/test mount' ] || exit 92
 mount_calls=$((mount_calls+1))
 [ "$mounted" = 1 ] || { [ "$mounted" = 2 ] && [ "$mount_calls" = 1 ]; }
}
sleep() { [ "$1" = 0.2 ] || exit 93; waits=$((waits+1)); }
'''+f+r'''
rc=0; client_wait_serving '/test mount' || rc=$?
echo "$rc $calls $waits"
'''
    result=subprocess.run(['bash','-c',script],text=True,capture_output=True,check=True)
    assert result.stdout.strip()==f'{expected} {probes} {waits}',result
assert 'if ! client_wait_serving "$MOUNT_PATH"; then' in s
print('client startup: immediate/late readiness, bounded exhaustion and detach PASS')
