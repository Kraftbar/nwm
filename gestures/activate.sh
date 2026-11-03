#!/bin/sh
# Install the swipe setup for this machine and start it. Needs passwordless
# sudo for the udev rule. Builds nothing: run `make gesture_detector` first.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
bin=$root/gesture_detector
if [ ! -x "$bin" ]; then
	echo "Build it first: make -C $root gesture_detector" >&2
	exit 1
fi
sudo -n install -m 644 "$root/gestures/70-nybo-touchpad.rules" /etc/udev/rules.d/70-nybo-touchpad.rules
sudo -n udevadm control --reload-rules
sudo -n udevadm trigger --subsystem-match=input --action=change
sudo -n udevadm settle
# Confirm access before switching away from Touchégg.
/usr/bin/timeout 3 "$bin" --dry-run > /tmp/nybo-gestures-check.log 2>&1 || test "$?" -eq 124
cat /tmp/nybo-gestures-check.log
if ! grep -q 'Touchpad connected:' /tmp/nybo-gestures-check.log; then
	echo 'Touchpad access check failed; Touchégg has been left unchanged.' >&2
	exit 1
fi
install -m 755 "$root/gestures/start-nybo-gestures" "$HOME/.local/bin/start-nybo-gestures"
install -m 644 "$root/gestures/nybo-gestures.desktop" "$HOME/.config/autostart/nybo-gestures.desktop"
install -m 644 "$root/gestures/touchegg.desktop" "$HOME/.config/autostart/touchegg.desktop"
unit=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/nybo-gestures.service
mkdir -p "$(dirname "$unit")"
sed "s|@BIN@|$bin|" "$root/gestures/nybo-gestures.service" > "$unit"
sudo -n systemctl disable --now touchegg.service
pkill -u "$(id -u)" -x touchegg || true
systemctl --user daemon-reload
"$HOME/.local/bin/start-nybo-gestures"
systemctl --user status nybo-gestures.service --no-pager
