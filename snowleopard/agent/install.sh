#!/bin/bash
# Install or update RV530Agent (drag and drop, clipboard,
# window-driven resolution) for the current user of the Snow Leopard
# guest.  Needs gcc from Xcode 3.2.  Run in the guest from this folder,
# from Terminal or over ssh:
#     ./install.sh             install, or update a running agent
#     ./install.sh --uninstall
# From the host, scripts/agent-update.sh copies this folder in and runs it.
# The VM must run with DND=1 / CLIPBOARD=1 (sl-vm.sh) for the agent to find
# the host.
set -e
cd "$(dirname "$0")"
DEST="$HOME/Library/Application Support/RV530Agent"
PLIST="$HOME/Library/LaunchAgents/org.rv530.agent.plist"
LABEL=org.rv530.agent

# Commands in the user's GUI (Aqua) session: LaunchAgents live there, and
# an ssh session has its own bootstrap namespace, where load/unload find
# nothing ("nothing found to load").  Over ssh, enter the namespace of the
# user's Dock (sudo: bsexec into another process needs root).
in_gui() {
    local dock
    if [ -z "$SSH_CONNECTION" ] && [ "$(launchctl managername 2>/dev/null)" = Aqua ]; then
        "$@"
        return
    fi
    dock=$(ps -axo pid=,user=,comm= | awk -v u="$USER" '$2 == u && /Dock\.app/ {print $1; exit}')
    if [ -z "$dock" ]; then
        echo "install.sh: $USER is not logged in to the GUI; it starts at the next login" >&2
        return 1
    fi
    if [ "$(id -u)" = 0 ]; then
        launchctl bsexec "$dock" sudo -u "$USER" "$@"
    else
        sudo -n launchctl bsexec "$dock" sudo -u "$USER" "$@" ||
            { echo "install.sh: needs passwordless sudo over ssh (or run it in Terminal)" >&2; return 1; }
    fi
}

gui_loaded() {
    in_gui launchctl list 2>/dev/null | grep -q "[[:space:]]$LABEL\$"
}

running_pids() {
    # the path has a space ("Application Support"): match the whole line
    ps -axo pid=,user=,comm= | awk -v u="$USER" '$2 == u && /\/RV530Agent$/ {print $1}'
}

if [ "$1" = --uninstall ]; then
    # remove by label: unload from a launchctl started over ssh does nothing
    in_gui launchctl remove "$LABEL" 2>/dev/null || true
    pids=$(running_pids)
    [ -z "$pids" ] || kill $pids 2>/dev/null || true
    rm -rf "$DEST" "$PLIST" "$HOME/Library/Caches/RV530Agent"
    echo "RV530Agent removed"
    exit 0
fi

mkdir -p "$DEST" "$HOME/Library/LaunchAgents"
# build next to the old binary and swap it in, so a running agent (or a
# KeepAlive restart) never sees a half-written file
if command -v gcc >/dev/null; then
    gcc -Wall -O2 -framework Cocoa -framework ApplicationServices -framework IOKit \
        -o "$DEST/RV530Agent.new" RV530Agent.m
elif [ -x RV530Agent ]; then
    # no Xcode in the guest: a binary built on a macOS host (build-host.sh)
    cp RV530Agent "$DEST/RV530Agent.new"
else
    echo "install.sh: needs gcc (Xcode 3.2), or RV530Agent built with build-host.sh" >&2
    exit 1
fi
mv -f "$DEST/RV530Agent.new" "$DEST/RV530Agent"
cat > "$PLIST.new" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$LABEL</string>
    <key>ProgramArguments</key><array><string>$DEST/RV530Agent</string></array>
    <key>LimitLoadToSessionType</key><string>Aqua</string>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
</dict>
</plist>
EOF

start_direct() {
    # launchctl started over ssh does not load into the Aqua session on
    # 10.6: run the agent there directly; launchd takes over at next login
    in_gui /bin/sh -c '"$0" </dev/null >/dev/null 2>&1 &' "$DEST/RV530Agent"
}

wait_running() {
    local i
    for ((i = 0; i < ${1:-15}; i++)); do
        [ -n "$(running_pids)" ] && [ "$(running_pids)" != "$2" ] && return 0
        sleep 1
    done
    return 1
}

pids=$(running_pids)
if [ -n "$pids" ] && [ -f "$PLIST" ] && cmp -s "$PLIST" "$PLIST.new"; then
    # update: restart the agent on the new binary
    rm -f "$PLIST.new"
    kill $pids
    if gui_loaded; then
        # launchd's KeepAlive restarts it (retries throttled to 10 s)
        wait_running 15 "$pids" && { echo "RV530Agent updated and restarted (pid $(running_pids))"; exit 0; }
    fi
    sleep 1
    start_direct && wait_running 5 && { echo "RV530Agent updated and restarted (pid $(running_pids))"; exit 0; }
    echo "install.sh: the agent did not come back; log out and in to start it" >&2
    exit 1
fi
mv -f "$PLIST.new" "$PLIST"
in_gui launchctl unload "$PLIST" 2>/dev/null || true
in_gui launchctl load "$PLIST" 2>/dev/null || true
if gui_loaded; then
    wait_running 15 || true
fi
if [ -n "$(running_pids)" ]; then
    echo "RV530Agent installed and started ($DEST)"
elif start_direct && wait_running 5; then
    echo "RV530Agent installed and started ($DEST); launchd keeps it running from the next login"
else
    echo "RV530Agent installed ($DEST); it starts at the next login"
fi
