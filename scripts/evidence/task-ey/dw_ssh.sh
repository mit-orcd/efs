#!/bin/bash
# DW_SSH for scripts/evidence/dw/k38_dw_campaign.py on the login node: HOST, script on stdin.
exec "$(dirname "$0")/task_ey_ssh.sh" "$1" bash -s
