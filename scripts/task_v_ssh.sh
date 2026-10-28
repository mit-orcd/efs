#!/bin/bash
# Task V private four-host driver wrapper. No password/token is supplied.
set -eu
host=$1; shift
case "$host" in fcstor012|fcstor013|fcstor014|fcstor015) ;; *) echo "Task V: refused host $host" >&2; exit 2;; esac
case "$(hostname -s)" in
  login*|fcstor012|fcstor013|fcstor014|fcstor015)
    exec ssh -A -o ConnectTimeout=20 -o ServerAliveInterval=30 "$host" "$@" ;;
  *)
    printf -v remote '%q ' ssh -A -o ConnectTimeout=20 -o ServerAliveInterval=30 "$host" "$@"
    exec ssh -A -o ConnectTimeout=20 -o ServerAliveInterval=30 orcd-login "$remote" ;;
esac
