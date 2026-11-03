CFLAGS ?= -O2 -Wall -Wextra -Wno-missing-field-initializers
PKGS = x11 xinerama xrandr xft fontconfig xcomposite xrender
PREFIX ?= /usr/local

nwm: nwm.c
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -o $@ nwm.c \
		$$(pkg-config --libs $(PKGS))

# Swipes are a separate program. X does not deliver them, so it opens the
# touchpad itself. `make` does not build it.
gesture_detector: gestures/gesture_detector.c
	$(CC) -std=c11 $(CFLAGS) -Werror $$(pkg-config --cflags libinput libudev) \
		-o $@ gestures/gesture_detector.c \
		$$(pkg-config --libs libinput libudev) -lm

check-gestures: gesture_detector
	./gesture_detector --self-test

# Nested in a window on the running desktop, on its own D-Bus: without
# that, Super+L in there locks the real screen.
run: nwm
	Xephyr :3 -ac -screen 1280x800 & sleep 1; DISPLAY=:3 dbus-run-session ./nwm -n

# Puts nwm on the login screen next to Cinnamon. Needs root.
install: nwm
	install -Dm755 nwm $(DESTDIR)$(PREFIX)/bin/nwm
	install -Dm644 nwm.desktop $(DESTDIR)/usr/share/xsessions/nwm.desktop

# Binary, touchpad read access, and an autostart entry for an nwm session.
# Reloads udev on this machine. Log in again so logind grants the ACL and
# autostart starts the daemon. Logging in alone does not apply a newly
# installed rule. Does not touch nwm itself.
install-gestures: gesture_detector
	install -Dm755 gesture_detector $(DESTDIR)$(PREFIX)/bin/nwm-gestures
	install -Dm644 gestures/70-touchpad.rules \
		$(DESTDIR)/etc/udev/rules.d/70-nwm-touchpad.rules
	install -Dm644 gestures/nwm-gestures.desktop \
		$(DESTDIR)/etc/xdg/autostart/nwm-gestures.desktop
	if [ -z "$(DESTDIR)" ]; then \
		udevadm control --reload-rules && \
		udevadm trigger --subsystem-match=input; \
	fi

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/nwm $(DESTDIR)/usr/share/xsessions/nwm.desktop

uninstall-gestures:
	rm -f $(DESTDIR)$(PREFIX)/bin/nwm-gestures \
		$(DESTDIR)/etc/udev/rules.d/70-nwm-touchpad.rules \
		$(DESTDIR)/etc/xdg/autostart/nwm-gestures.desktop

clean:
	rm -f nwm gesture_detector

.PHONY: run install install-gestures uninstall uninstall-gestures clean check-gestures
