#!/usr/bin/env python3
"""Visible EFS qualification owner; forward signals only to our own driver."""
import signal
import subprocess
import sys

child = subprocess.Popen(['bash', *sys.argv[1:]])
def forward(sig, _frame):
    if child.poll() is None:
        child.send_signal(sig)
for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
    signal.signal(sig, forward)
raise SystemExit(child.wait())
