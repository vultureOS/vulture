#!/bin/sh
#
# Copyright (c) 2022-2026, Krisna Pranav
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Run this once, in Terminal, inside the macOS 15 virtual machine, logged in. From then on, whatever the host puts in the
# shared folder's requests/ is run in this person's session, where AppKit can draw and the screen can be read, and what it
# says goes in results/. That's all this installs; removing ~/Library/LaunchAgents/com.vulture.calibration.plist undoes it.

set -eu

share="/Volumes/My Shared Files/calibration"
if [ ! -d "$share" ]; then
    echo "guest-setup: there's no $share: was the virtual machine started with Meta/Calibration/macos-vm.sh run?" >&2
    exit 1
fi

agent_dir="$HOME/Library/Application Support/vulture-calibration"
mkdir -p "$agent_dir" "$HOME/Library/LaunchAgents"
cat > "$agent_dir/runner.sh" <<'RUNNER'
#!/bin/sh
share="/Volumes/My Shared Files/calibration"
while :; do
    for request in "$share"/requests/*.sh; do
        [ -f "$request" ] || continue
        name="$(basename "$request" .sh)"
        mkdir -p "$share/results"
        (cd "$share" && /bin/sh "$request") > "$share/results/$name.out" 2>&1
        echo "$?" > "$share/results/$name.status"
        mv "$request" "$share/results/$name.ran"
    done
    sleep 1
done
RUNNER
chmod +x "$agent_dir/runner.sh"
cat > "$HOME/Library/LaunchAgents/com.vulture.calibration.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>com.vulture.calibration</string>
    <key>ProgramArguments</key><array><string>$agent_dir/runner.sh</string></array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>LimitLoadToSessionType</key><string>Aqua</string>
</dict>
</plist>
PLIST
launchctl unload "$HOME/Library/LaunchAgents/com.vulture.calibration.plist" 2>/dev/null || true
launchctl load "$HOME/Library/LaunchAgents/com.vulture.calibration.plist"
echo "guest-setup: ready: the host can run things here now. This is macOS $(sw_vers -productVersion)."
