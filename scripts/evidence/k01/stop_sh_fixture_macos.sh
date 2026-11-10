#!/bin/bash
# stop_sh_fixture_macos.sh TEST_PY STOP_SH
# Run devops nuc-efs/test_daemon_ownership.py on macOS against the given stop.sh,
# with the Linux guard removed from a temporary copy of the test and the
# macOS stand-in server_processes_macos_stub.py as the helper. Prints the
# test's last lines; exit status is the test's. Touches only a temp directory.
#   git -C ~/git/devops show devel:nuc-efs/stop.sh > /tmp/stop.old.sh
#   git -C ~/git/devops show k01b-stop-nonorderly:nuc-efs/stop.sh > /tmp/stop.new.sh
#   git -C ~/git/devops show k01b-stop-nonorderly:nuc-efs/test_daemon_ownership.py > /tmp/test.new.py
#   scripts/evidence/k01/stop_sh_fixture_macos.sh /tmp/test.new.py /tmp/stop.old.sh   # FAIL
#   scripts/evidence/k01/stop_sh_fixture_macos.sh /tmp/test.new.py /tmp/stop.new.sh   # PASS
set -eu
here=$(cd "$(dirname "$0")" && pwd)
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
mkdir -p "$t/efs/scripts"
cp "$here/server_processes_macos_stub.py" "$t/efs/scripts/server_processes.py"
sed "s/sys.platform != 'linux'/False/" "$1" > "$t/test_daemon_ownership.py"
cp "$2" "$t/stop.sh"
cd "$t" && python3 test_daemon_ownership.py --efs-source "$t/efs" 2>&1 | tail -4; exit "${PIPESTATUS[0]}"
