# Binary streams are single-attempt. Extract away from installed executables;
# verify the whole archive before renaming files into place (avoids ETXTBSY).
ssh_stream_once() { ssh_node_once "$@"; }
ship_binaries() {  # source VM, destination VM
    local bins='efsd efs-fuse efs-mgmt efs-query efs-bench'
    ssh_stream_once "$1" "cd /tmp/efs && tar -cf - $bins" |
        ssh_stream_once "$2" 'set -eo pipefail
            mkdir -p /tmp/efs
            stage=$(mktemp -d /tmp/efs/.deploy.XXXXXX)
            trap '\''rm -rf "$stage"'\'' EXIT
            tar -xf - -C "$stage"
            for b in efsd efs-fuse efs-mgmt efs-query efs-bench; do
                test -x "$stage/$b" || exit 1
            done
            for b in efsd efs-fuse efs-mgmt efs-query efs-bench; do
                mv -f "$stage/$b" "/tmp/efs/$b"
            done'
}

# Profiling clients need the entry point and matching C source, not just binaries.
ship_bench_harness() { # destination VM, local EFS source root
    local node=$1 src=$2 path
    for path in efs-bench.sh Makefile scripts/bench_profile.py src include deps/blake3; do
        test -e "$src/$path" || { echo "missing benchmark harness input: $src/$path" >&2; return 1; }
    done
    ssh_node_once "$node" 'mkdir -p /tmp/efs/scripts /tmp/efs/src /tmp/efs/include /tmp/efs/deps/blake3'
    rsync_to "$node" "$src/efs-bench.sh" /tmp/efs/
    rsync_to "$node" "$src/Makefile" /tmp/efs/
    rsync_to "$node" "$src/scripts/bench_profile.py" /tmp/efs/scripts/
    for path in src include deps/blake3; do
        rsync_to "$node" "$src/$path/" "/tmp/efs/$path/" \
            --include '*/' --include '*.c' --include '*.h' --include '*.S' --include '*.s' --exclude '*'
    done
}
