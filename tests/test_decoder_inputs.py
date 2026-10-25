#!/usr/bin/env python3
"""Task C deterministic byte mutations, including caller-state invariance."""
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[1]
TARGETS = ('membership', 'member-view', 'discovery-probe', 'discovery-answer',
           'boot', 'identity', 'fragment', 'fragment-verify', 'ticket', 'ticket-batch', 'ticket-settle',
           'hello', 'recovery-request', 'recovery-reply', 'audit-request', 'audit-reply',
           'degraded', 'degraded-command', 'recovery-command', 'pack-degraded', 'ticket-drain', 'put-header', 'cleanup-probe', 'receipt', 'declaration')
if __name__ == '__main__':
    subprocess.run([str(ROOT/'tests/decoders/apply')], check=True)
    for target in TARGETS:
        subprocess.run([str(ROOT/'tests/decoders/check'), target], check=True)
