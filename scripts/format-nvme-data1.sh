#!/bin/bash
# Format all data NVMe namespaces as XFS (mkfs.xfs -K -f), label them, and
# mount at /data1/01..14 via fstab LABEL=. First mount: chmod 1777.
#
# Default is a dry-run. Destructive format requires --yes.
#
# Usage (as root on the storage node):
#   ./scripts/format-nvme-data1.sh           # show plan
#   ./scripts/format-nvme-data1.sh --yes     # format, fstab, mount, chmod
#
# Labels: DATA1_01 .. DATA1_14  (XFS label max 12 chars)
# Skips the device that backs / (and any other mounted non-data1 NVMe).
#
# XFS tuned for efs: many create+O_DIRECT fragment files (64 KiB..tens of MiB),
# many writer threads, no snapshots/reflink. See comment on MKFS_XFS_OPTS.

set -euo pipefail

YES=0
COUNT=14
PREFIX="/data1"
LABEL_FMT="DATA1_%02d"
FSTAB_MARK="# efs-data1 NVMe (format-nvme-data1.sh)"
# -K: skip discard (hours on 7 TB). -m: no reflink/rmap (efs never uses them).
# -d agcount=16: parallel allocs on ~7.6 TB NVMe. -l 128m: creat/unlink bursts.
MKFS_XFS_OPTS="-K -f -m crc=1,finobt=1,reflink=0,rmapbt=0 -d agcount=16 -l size=128m"
# logbufs/logbsize: more in-core journal for mkdir/creat storms.
FSTAB_OPTS="defaults,noatime,nofail,logbufs=8,logbsize=256k"

usage() {
    sed -n '2,16p' "$0" | sed 's/^# \?//'
    exit "${1:-0}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --yes) YES=1 ;;
        -h|--help) usage 0 ;;
        *) echo "Unknown argument: $1" >&2; usage 1 ;;
    esac
    shift
done

if [ "$(id -u)" -ne 0 ]; then
    echo "Must run as root (dry-run still needs lsblk)." >&2
    exit 1
fi

command -v mkfs.xfs >/dev/null || { echo "mkfs.xfs not found" >&2; exit 1; }

root_src=$(findmnt -n -o SOURCE / || true)
root_pk=$(lsblk -no PKNAME "$root_src" 2>/dev/null | head -1 || true)
root_dev=$(readlink -f "$root_src" 2>/dev/null || echo "$root_src")

# nvme0n1 .. nvme13n1 sorted numerically (not lexical 0,1,10,11).
mapfile -t CANDS < <(
    ls -1 /dev/nvme*n1 2>/dev/null | awk '
        { if (match($0, /nvme([0-9]+)n1$/, a)) printf "%d %s\n", a[1], $0 }
    ' | sort -n | awk '{print $2}'
)

DEVS=()
for d in "${CANDS[@]}"; do
    [ -b "$d" ] || continue
    real=$(readlink -f "$d")
    if [ -n "$root_dev" ] && [ "$real" = "$root_dev" ]; then
        echo "skip OS disk $d (mounted on /)"
        continue
    fi
    pk=$(lsblk -no PKNAME "$d" 2>/dev/null | head -1 || true)
    if [ -n "$root_pk" ] && [ -n "$pk" ] && [ "$pk" = "$root_pk" ]; then
        echo "skip $d (same controller as /)"
        continue
    fi
    # Skip if this namespace (or a partition on it) is mounted somewhere
    # other than the target /data1/NN we are about to own.
    mps=$(findmnt -n -o TARGET --source "$d" 2>/dev/null || true)
    if [ -z "$mps" ]; then
        # partitions e.g. nvme0n1p1
        for p in "${d}"p*; do
            [ -b "$p" ] || continue
            extra=$(findmnt -n -o TARGET --source "$p" 2>/dev/null || true)
            [ -n "$extra" ] && mps="$mps $extra"
        done
    fi
    skip=0
    for mp in $mps; do
        case "$mp" in
            ${PREFIX}/[0-9][0-9]) ;;
            *)
                echo "skip $d (mounted at $mp)"
                skip=1
                ;;
        esac
    done
    [ "$skip" -eq 1 ] && continue
    DEVS+=("$d")
done

if [ "${#DEVS[@]}" -lt "$COUNT" ]; then
    echo "Need $COUNT data NVMe namespaces; found ${#DEVS[@]}:" >&2
    printf '  %s\n' "${DEVS[@]:-}" >&2
    exit 1
fi
if [ "${#DEVS[@]}" -gt "$COUNT" ]; then
    echo "Found ${#DEVS[@]} candidates; using first $COUNT (numeric nvme order)."
    DEVS=("${DEVS[@]:0:$COUNT}")
fi

echo
echo "=== plan ($([ "$YES" -eq 1 ] && echo APPLY || echo DRY-RUN)) ==="
for i in $(seq 1 "$COUNT"); do
    idx=$((i - 1))
    lab=$(printf "$LABEL_FMT" "$i")
    mp=$(printf "%s/%02d" "$PREFIX" "$i")
    printf '  %s  ->  LABEL=%s  %s\n' "${DEVS[$idx]}" "$lab" "$mp"
done
echo

if [ "$YES" -ne 1 ]; then
    echo "Re-run with --yes to mkfs.xfs -K -f, write fstab, mount, chmod 1777."
    exit 0
fi

# Unmount existing /data1/NN so -f format can proceed.
for i in $(seq 1 "$COUNT"); do
    mp=$(printf "%s/%02d" "$PREFIX" "$i")
    if findmnt -n "$mp" >/dev/null 2>&1; then
        echo "umount $mp"
        umount "$mp"
    fi
done

for i in $(seq 1 "$COUNT"); do
    idx=$((i - 1))
    dev="${DEVS[$idx]}"
    lab=$(printf "$LABEL_FMT" "$i")
    mp=$(printf "%s/%02d" "$PREFIX" "$i")

    echo "mkfs.xfs $MKFS_XFS_OPTS -L $lab $dev"
    # shellcheck disable=SC2086
    mkfs.xfs $MKFS_XFS_OPTS -L "$lab" "$dev"

    mkdir -p "$mp"
    line="LABEL=${lab}  ${mp}  xfs  ${FSTAB_OPTS}  0  0"
    if grep -qE "[[:space:]]${mp}[[:space:]]" /etc/fstab 2>/dev/null; then
        tmp=$(mktemp)
        awk -v mp="$mp" -v repl="$line" '
            $2 == mp { print repl; next }
            { print }
        ' /etc/fstab > "$tmp"
        cat "$tmp" > /etc/fstab
        rm -f "$tmp"
        echo "fstab updated $mp"
    else
        if ! grep -qxF "$FSTAB_MARK" /etc/fstab 2>/dev/null; then
            printf '\n%s\n' "$FSTAB_MARK" >> /etc/fstab
        fi
        echo "$line" >> /etc/fstab
        echo "fstab += $line"
    fi

    mount "$mp"
    chmod 1777 "$mp"
    echo "mounted $mp mode=$(stat -c %a "$mp")"
done

echo
echo "done. findmnt $PREFIX:"
findmnt -R "$PREFIX" || ls -ld "$PREFIX"/*