#!/bin/bash
set -euo pipefail
mkdir -p /private/tmp/w92-ssh-bin
cat > /private/tmp/w92-ssh-bin/ssh <<'WRAPPER'
#!/bin/bash
exec /usr/bin/ssh -i /Users/mike/git/cluster/cluster_key "$@"
WRAPPER
chmod 755 /private/tmp/w92-ssh-bin/ssh
