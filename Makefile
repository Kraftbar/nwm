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

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/nwm $(DESTDIR)/usr/share/xsessions/nwm.desktop

clean:
	rm -f nwm gesture_detector

.PHONY: run install uninstall clean check-gestures
