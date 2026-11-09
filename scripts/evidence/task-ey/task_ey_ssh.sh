#!/bin/bash
# Task EY ssh wrapper for qualify.py ("ssh" in the cluster description): PROG HOST COMMAND.
# The driver runs on the ORCD login node, which reaches fcstor010-014 by key; the fcstor
# hosts cannot ssh onward. Only the five batch D hosts are accepted; no password is typed.
set -eu
host=$1; shift
case "$host" in fcstor010|fcstor011|fcstor012|fcstor013|fcstor014) ;; *) echo "Task EY: refused host $host" >&2; exit 2;; esac
exec ssh -o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=30 "$host" "$@"
