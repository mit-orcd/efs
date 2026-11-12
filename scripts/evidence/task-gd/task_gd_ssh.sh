#!/bin/bash
# Task GD ssh wrapper for qualify.py ("ssh" in the cluster description): PROG HOST COMMAND.
# The controller runs on the Mac and reaches fcstor010-014 as erbmi1 through the one shared mux socket
# to orcd-login (-W forwarding; this script never opens a connection to the login node of its own).
# One persistent master per host in a mode-700 directory on the Mac; no password is ever typed (BatchMode).
set -u
host=$1; shift
case "$host" in fcstor010|fcstor011|fcstor012|fcstor013|fcstor014) ;; *) echo "Task GD: refused host $host" >&2; exit 2;; esac
sock="${GD_MUX_SOCKET:-$HOME/git/cluster/state/orcd-mini/.ssh-mux-erbmi1@orcd-login.mit.edu:22}"
[ -S "$sock" ] || { echo "Task GD: shared mux socket missing, not dialling" >&2; exit 3; }
mux=/tmp/efs-taskGD-mux-$(id -un); mkdir -p -m 700 "$mux"
ssh -l erbmi1 -o BatchMode=yes -o ConnectTimeout=30 -o ServerAliveInterval=30 -o ControlMaster=auto \
    -o ControlPath="$mux/%h" -o ControlPersist=12h -o UserKnownHostsFile="$HOME/.ssh/known_hosts $mux/known_hosts" \
    -o ProxyCommand="ssh -o ControlPath='$sock' -o BatchMode=yes -W %h:%p orcd-login" "$host" "$@" \
    2> >(grep -v -e '\.bashrc: line [0-9]*: .*cargo/env: No such file or directory' >&2)
rc=$?
wait
exit $rc
