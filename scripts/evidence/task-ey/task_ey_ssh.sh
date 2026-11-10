#!/bin/bash
# Task EY ssh wrapper for qualify.py ("ssh" in the cluster description): PROG HOST COMMAND.
# The driver runs on the ORCD login node, which reaches fcstor010-014 by key; the fcstor
# hosts cannot ssh onward. Only the five batch D hosts are accepted; no password is typed.
# The account's ~/.bashrc prints two "cargo/env: No such file or directory" lines on every
# non-interactive ssh; they are dropped from stderr so version reads (identity gate) parse.
set -eu
host=$1; shift
case "$host" in fcstor010|fcstor011|fcstor012|fcstor013|fcstor014) ;; *) echo "Task EY: refused host $host" >&2; exit 2;; esac
# One multiplexed master connection per host (local /tmp on the controller; sockets do not work on NFS), so
# bursts of driver commands neither re-authenticate through the forwarded agent nor hit sshd's MaxStartups.
mux=/tmp/efs-taskEY-mux-$(id -un); mkdir -p -m 700 "$mux"
ssh -o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=30 -o ControlMaster=auto \
    -o ControlPath="$mux/%h" -o ControlPersist=600 "$host" "$@" \
    2> >(grep -v -e '\.bashrc: line [0-9]*: .*cargo/env: No such file or directory' >&2)
rc=$?
wait
exit $rc
