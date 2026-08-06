# Shared InfiniBand address helper. Source from server/client job scripts.
# Convention on Engaging: <short-hostname>.ib resolves to the IB IPoIB address.

efs_ib_host() {
    local short
    short=$(hostname -s)
    local ib="${short}.ib"
    # Prefer resolving to an IPv4 so logs and /dev/tcp checks are unambiguous.
    local ip
    ip=$(getent ahostsv4 "$ib" 2>/dev/null | awk '{print $1; exit}')
    if [ -z "$ip" ]; then
        ip=$(getent hosts "$ib" 2>/dev/null | awk '{print $1; exit}')
    fi
    if [ -z "$ip" ]; then
        echo "ERROR: cannot resolve InfiniBand host '$ib'" >&2
        echo "HINT: compute nodes should resolve \$(hostname -s).ib" >&2
        return 1
    fi
    # Echo "host ip" — callers use host for --addr (keeps .ib in cluster
    # membership) or ip for bind if preferred. We advertise the .ib name.
    printf '%s %s\n' "$ib" "$ip"
}
