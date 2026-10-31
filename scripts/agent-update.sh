#!/bin/bash
# Install or update the Snow Leopard guest agent (snowleopard/agent) from
# the host, over ssh: copies the source in, builds it with the
# guest's gcc and restarts it in the login session.
#     scripts/agent-update.sh [--uninstall]
# SSHPORT as for sl-vm.sh; snowleopard/sl-ssh.sh does the connecting.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
tar cf - -C "$HERE/snowleopard" agent |
    "$HERE/snowleopard/sl-ssh.sh" 'rm -rf /tmp/rv530-agent && mkdir -p /tmp/rv530-agent &&
        tar xf - -C /tmp/rv530-agent && chmod +x /tmp/rv530-agent/agent/install.sh'
"$HERE/snowleopard/sl-ssh.sh" "/tmp/rv530-agent/agent/install.sh $*" </dev/null
