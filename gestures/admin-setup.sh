#!/bin/sh
# Root half of activate.sh: the udev rule, then stop Touchégg once the
# desktop user can read the pad.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
install -m 644 "$root/gestures/70-nybo-touchpad.rules" /etc/udev/rules.d/70-nybo-touchpad.rules
udevadm control --reload-rules
udevadm trigger --subsystem-match=input --action=change
udevadm settle
accessible=false
for device in /dev/input/event*; do
	if udevadm info -q property -n "$device" | grep -qx 'ID_INPUT_TOUCHPAD=1' && runuser -u nybo -- test -r "$device"; then
		accessible=true
	fi
done
if [ "$accessible" != true ]; then
	echo 'No accessible touchpad; leaving Touchégg running.' >&2
	exit 1
fi
systemctl disable --now touchegg.service
