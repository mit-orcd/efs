#!/bin/sh
# Use graphify's own Python, not an unrelated system environment.
set -eu
cd "$(dirname "$0")/.."
launcher=$(command -v graphify) || { echo 'Install graphifyy==0.9.97 first' >&2; exit 1; }
interpreter=$(head -n 1 "$launcher")
interpreter=${interpreter#\#!}
if [ ! -x "$interpreter" ]; then
    echo 'Cannot resolve graphify Python; run scripts/build-code-graph.py in its environment' >&2
    exit 1
fi
PYTHONHASHSEED=0 "$interpreter" scripts/build-code-graph.py "$@"
