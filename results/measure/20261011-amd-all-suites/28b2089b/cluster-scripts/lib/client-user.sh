# Provisioning may use admin/root; all EFS runtime commands use efs.
ensure_efs_user() {  # VM
    ssh_node_once "$1" 'set -e; id efs >/dev/null 2>&1 || sudo -n useradd -m -s /bin/bash efs; test "$(id -u efs)" -ne 0'
}

# Prepare only local mount scaffolding as root; EFS itself runs as efs.
prepare_client_mount() {  # VM, absolute mountpoint
    local script command
    IFS= read -r -d '' script <<'REMOTE' || true
set -eo pipefail
mnt=$1
[ "$(id -u)" -ne 0 ] || { echo 'ERROR: EFS must not run as root' >&2; exit 1; }
case "$mnt" in /mnt/efs|/mnt/efs-b) ;; *) echo 'ERROR: unsupported VM mountpoint' >&2; exit 1;; esac
mountpoint -q "$mnt" && { echo 'ERROR: stop the existing mount before preparation' >&2; exit 1; }
id efs >/dev/null 2>&1 || sudo -n useradd -m -s /bin/bash efs
u=$(id -u efs); g=$(id -g efs)
[ "$u" -ne 0 ] || { echo "ERROR: efs must not have UID 0" >&2; exit 1; }
# Do not remove/recreate a lock inode: a detached client may still hold it.
sudo -n install -d -m 0755 -o "$u" -g "$g" "$mnt"
for file in "/mnt/.efs-$(basename "$mnt").lock" "/mnt/efs-fuse-$(basename "$mnt").log"; do
    [ ! -L "$file" ] || { echo "ERROR: refusing symlink $file" >&2; exit 1; }
    sudo -n touch "$file"
    sudo -n chown "$u:$g" "$file"
done
# Non-root allow_other requires the explicit FUSE configuration opt-in.
if ! grep -Eq '^[[:space:]]*user_allow_other([[:space:]]*(#.*)?)?$' /etc/fuse.conf 2>/dev/null; then
    printf '\nuser_allow_other\n' | sudo -n tee -a /etc/fuse.conf >/dev/null
fi
command -v fusermount3 >/dev/null
REMOTE
    printf -v command 'bash -c %q -- %q' "$script" "$2"
    ssh_node_once "$1" "$command"
}

# One-time migration for stopped legacy stores; ordinary starts avoid a tree scan.
prepare_server_storage() {  # VM, dedicated storage mountpoint
    local script command
    IFS= read -r -d '' script <<'REMOTE' || true
set -eo pipefail
dir=$1
case "$dir" in /*) ;; *) echo 'ERROR: storage path must be absolute' >&2; exit 1;; esac
[ "$dir" != / ] && [ "$(readlink -f "$dir")" = "$dir" ] || exit 1
[ "$(findmnt -R "$dir" -n -o TARGET | wc -l)" -eq 1 ] || { echo 'ERROR: expected a dedicated storage filesystem' >&2; exit 1; }
mountpoint -q "$dir" || exit 1
for process in efsd efs-fuse; do
    rc=0; pgrep -x "$process" >/dev/null || rc=$?
    [ "$rc" -eq 1 ] || { echo "ERROR: $process running or discovery failed; drain and stop first" >&2; exit 1; }
done
[ "$(id -u efs)" -ne 0 ] || exit 1
if [ "$(stat -c %U:%G "$dir")" != efs:efs ]; then
    echo 'Migrating stopped legacy EFS storage to efs:efs'
    sudo -n chown -R --no-dereference --preserve-root efs:efs "$dir"
fi
sudo -n -u efs test -w "$dir"
REMOTE
    printf -v command 'bash -c %q -- %q' "$script" "$2"
    ssh_node_once "$1" "$command"
}
