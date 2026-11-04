#!/bin/bash
# Private matching runtime/header extraction; never install system packages.
set -euo pipefail
root=${1:?private dependency directory required}
[ "$(hostname -s)" = fcstor014 ]
case "$root" in /tmp/efs-taskAN-deps|/tmp/efs-taskAN-deps/*) ;; *) exit 2;; esac
[ ! -e "$root/tools" ] || { echo 'refusing to overwrite existing private tools' >&2; exit 2; }
mkdir -p "$root/rpms" "$root/query"
gcc_version=$(rpm -q gcc --qf '%{VERSION}-%{RELEASE}.%{ARCH}')
vg_version=$(rpm -q valgrind --qf '%{VERSION}-%{RELEASE}.%{ARCH}')
runtime=libtsan-$gcc_version
headers=valgrind-devel-$vg_version
if [ ! -f "$root/rpms/$runtime.rpm" ] || [ ! -f "$root/rpms/$headers.rpm" ]; then
    dnf --setopt="logfile=$root/query/download.log" \
        --setopt="persistdir=$root/query/persist" download --destdir "$root/rpms" \
        --arch=x86_64 "$runtime" "$headers" > "$root/query/download.out" 2>&1
fi
for package in "$runtime" "$headers"; do
    rpmkeys --checksig --verbose "$root/rpms/$package.rpm" > "$root/query/$package.signature" 2>&1
    rpm2cpio "$root/rpms/$package.rpm" | cpio -t > "$root/query/$package.files" 2> "$root/query/$package.list.log"
done
python3 - "$root" "$runtime" "$headers" <<'PY'
from pathlib import Path, PurePosixPath
import sys
import re
root=Path(sys.argv[1])
for name in sys.argv[2:]:
    signature=(root/'query'/f'{name}.signature').read_text()
    assert re.search(r'Signature.*:\s*OK',signature),signature
    for entry in (root/'query'/f'{name}.files').read_text().splitlines():
        path=PurePosixPath(entry)
        assert not path.is_absolute() and '..' not in path.parts,entry
PY
mkdir "$root/tools"
for package in "$runtime" "$headers"; do
    (cd "$root/tools"; rpm2cpio "$root/rpms/$package.rpm" | cpio -idm --no-absolute-filenames) \
        > "$root/query/$package.extract.log" 2>&1
done
python3 - "$root/tools" <<'PY'
from pathlib import Path
import sys
tools=Path(sys.argv[1]).resolve()
assert (tools/'usr/lib64/libtsan.so.0').is_file()
assert (tools/'usr/include/valgrind/memcheck.h').is_file()
(tools/'usr/lib64/libtsan.so').symlink_to('libtsan.so.0')
(tools/'bin').mkdir()
wrapper=tools/'bin/gcc'
wrapper.write_text('#!/bin/sh\nfor arg in "$@"; do\n case "$arg" in *fsanitize*thread*)\n'
                  f'  exec /usr/bin/gcc -L"{tools}/usr/lib64" -Wl,-rpath,"{tools}/usr/lib64" "$@" ;;\n'
                  ' esac\ndone\nexec /usr/bin/gcc "$@"\n')
wrapper.chmod(0o755)
(tools/'bin/cc').symlink_to('gcc')
PY
(cd "$root"; sha256sum "rpms/$runtime.rpm" "rpms/$headers.rpm" \
    tools/bin/gcc tools/usr/lib64/libtsan.so.0.0.0 > PACKAGE_SHA256SUMS)
printf 'verified private dependencies: %s/tools\n' "$root"
