#!/bin/bash
# Bounded retry for the existing 30-second launch readiness window. Never stop,
# replace or discard an existing daemon/store; start.sh is idempotent.
set -u
for attempt in 1 2 3 4 5; do
 if /data1/efs/devops/start.sh; then exit 0; fi
 echo "start readiness attempt $attempt failed; retaining processes for bounded retry" >&2
 [ "$attempt" = 5 ] || sleep 5
done
exit 1
