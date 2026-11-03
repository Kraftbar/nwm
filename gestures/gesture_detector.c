#define _POSIX_C_SOURCE 200809L
#include <libinput.h>
#include <libudev.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

/* Threshold tuned from this touchpad: deliberate swipes measured 80-120 units. */
#define SWIPE_THRESHOLD 60.0
/* Three fingers sideways hold nwm's Alt+Tab switcher open: it shows after
 * SWITCH_START units, the selection moves every SWITCH_STEP, lifting picks. */
#define SWITCH_START 8.0
#define SWITCH_STEP 40.0
struct swipe { bool active, switching; double x, y, travel, travel_y, anchor; int fingers; };
static void update(struct swipe *s, double x, double y) {
    if (s->active) { s->x += x; s->y += y; s->travel += fabs(x); s->travel_y += fabs(y); }
}
static int finish(struct swipe *s, bool cancelled) {
    int direction = 0;
    if (s->active && !cancelled && fabs(s->x) >= SWIPE_THRESHOLD &&
        fabs(s->x) >= 2 * fabs(s->y) && fabs(s->x) >= .8 * s->travel)
        direction = s->x > 0 ? 1 : -1;
    if (s->active && !cancelled && s->fingers == 3 && fabs(s->y) >= SWIPE_THRESHOLD &&
        fabs(s->y) >= 2 * fabs(s->x) && fabs(s->y) >= .8 * s->travel_y)
        direction = s->y > 0 ? 2 : -2;
    *s = (struct swipe){0};
    return direction;
}
/* Finger-scroll sequences use separate units from gesture swipes. */
static bool finish_back(struct swipe *s) {
    bool back = s->active && s->fingers == 2 && s->x >= 60 &&
                s->x >= 4 * s->travel_y && s->x >= .9 * s->travel;
    *s = (struct swipe){0};
    return back;
}
static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }
static int open_input(const char *path, int flags, void *data) {
    (void)data;
    /* Only open touchpads, never keyboards or other input devices. */
    struct udev *u = udev_new();
    if (!u) return -ENOMEM;
    const char *name = strrchr(path, '/');
    struct udev_device *d = udev_device_new_from_subsystem_sysname(u, "input", name ? name + 1 : path);
    const char *touchpad = d ? udev_device_get_property_value(d, "ID_INPUT_TOUCHPAD") : NULL;
    bool allowed = touchpad && strcmp(touchpad, "1") == 0;
    if (d) udev_device_unref(d);
    udev_unref(u);
    if (!allowed) return -EACCES;
    int fd = open(path, flags | O_CLOEXEC);
    if (fd < 0) { int e = errno; fprintf(stderr, "%s: %s\n", path, strerror(e)); return -e; }
    return fd;
}
static void close_input(int fd, void *data) { (void)data; close(fd); }
static const struct libinput_interface interface = { open_input, close_input };
static bool dry_run, alt_down;
static void xdo(const char *action, const char *key) {
    if (dry_run) { printf("xdotool %s %s (dry run)\n", action, key); return; }
    pid_t child = fork();
    if (child == 0) {
        execl("/usr/bin/xdotool", "xdotool", action, key, (char *)NULL);
        _exit(127);
    }
    if (child < 0) { perror("fork"); return; }
    int status;
    pid_t result;
    do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status))
        fprintf(stderr, "xdotool %s %s failed\n", action, key);
}
static void live_switch(struct swipe *s) {
    if (!s->switching) {
        if (fabs(s->x) < SWITCH_START || fabs(s->x) < 2 * fabs(s->y)) return;
        s->switching = alt_down = true;
        s->anchor = s->x;
        xdo("keydown", "alt");
        xdo("key", s->x > 0 ? "Tab" : "shift+Tab");
        return;
    }
    for (; s->x - s->anchor >= SWITCH_STEP; s->anchor += SWITCH_STEP) xdo("key", "Right");
    for (; s->anchor - s->x >= SWITCH_STEP; s->anchor -= SWITCH_STEP) xdo("key", "Left");
}
static void end_switch(bool cancelled) {
    if (!alt_down) return;
    if (cancelled) xdo("key", "Escape");
    xdo("keyup", "alt");
    alt_down = false;
}
/* Two fingers right: Back. Three fingers up/down: nwm's snap ladder. */
static void trigger(int direction, int fingers) {
    const char *key = fingers == 2 ? "alt+Left" : direction == 2 ? "super+Down" : "super+Up";
    printf("Completed %d-finger swipe: %s\n", fingers, key);
    xdo("key", key);
}
static int self_test(void) {
    struct { double x, y, reverse; bool cancel; int expected; } cases[] = {
        {-400,0,0,false,-1}, {400,0,0,false,1}, {30,0,0,false,0}, {81.8,-2.8,0,false,1}, {-119.7,-1,0,false,-1}, {59,0,0,false,0}, {60,0,0,false,1},
        {400,300,0,false,0}, {400,0,0,true,0}, {600,0,-250,false,0}, {0,-400,0,false,-2}, {0,400,0,false,2}, {0,400,0,true,0}
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        struct swipe s = {.active = true, .fingers = 3};
        update(&s, cases[i].x / 2, cases[i].y / 2);
        update(&s, cases[i].x / 2, cases[i].y / 2);
        update(&s, cases[i].reverse, 0);
        if (finish(&s, cases[i].cancel) != cases[i].expected || finish(&s, false) != 0) return 1;
    }
    struct swipe four = {.active = true, .fingers = 4};
    update(&four, 0, 500);
    if (finish(&four, false)) return 1;
    four = (struct swipe){.active = true, .fingers = 4};
    update(&four, -500, 0);
    if (finish(&four, false) != -1) return 1;
    struct swipe inactive = {0};
    update(&inactive, 500, 0);
    if (finish(&inactive, false)) return 1;
    struct { double x, y, reverse; bool expected; } scrolls[] = {
        {80,0,0,true}, {-80,0,0,false}, {30,0,0,false},
        {80,30,0,false}, {120,0,-30,false}, {0,200,0,false}
    };
    for (size_t i = 0; i < sizeof scrolls / sizeof *scrolls; ++i) {
        struct swipe s = {.active = true, .fingers = 2};
        update(&s, scrolls[i].x, scrolls[i].y);
        update(&s, scrolls[i].reverse, 0);
        if (finish_back(&s) != scrolls[i].expected || finish_back(&s)) return 1;
    }
    /* Vertical travel cannot be hidden by returning to the starting height. */
    struct swipe scroll = {.active = true, .fingers = 2};
    update(&scroll, 40, 50); update(&scroll, 40, -50);
    if (finish_back(&scroll)) return 1;
    puts("Passed: swipe direction, threshold, cancellation, reversal, duplicate-end, and two-finger Back checks");
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--self-test")) return self_test();
    if (argc == 2 && !strcmp(argv[1], "--dry-run")) dry_run = true;
    else if (argc != 1) { fprintf(stderr, "Usage: %s [--dry-run|--self-test]\n", argv[0]); return 2; }
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || (!dry_run && !getenv("DISPLAY"))) { fputs("Run in your X11 desktop session\n", stderr); return 1; }
    char lockpath[4096];
    if (snprintf(lockpath, sizeof lockpath, "%s/nybo-gestures.lock", runtime) >= (int)sizeof lockpath) return 1;
    int lock = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) { fputs("Cannot lock; another instance may be running\n", stderr); return 1; }
    struct udev *u = udev_new();
    if (!u) return 1;
    struct libinput *li = libinput_udev_create_context(&interface, NULL, u);
    if (!li || libinput_udev_assign_seat(li, "seat0") < 0) { fputs("Cannot initialize input seat\n", stderr); return 1; }
    signal(SIGINT, stop); signal(SIGTERM, stop);
    setvbuf(stdout, NULL, _IOLBF, 0);
    puts(dry_run ? "Listening in dry-run mode; no keys will be sent" : "Listening for two-finger Back and three-finger swipes");
    struct pollfd fd = {.fd = libinput_get_fd(li), .events = POLLIN};
    int result = 0;
    while (running) {
        if (libinput_dispatch(li) < 0) { result = 1; break; }
        struct libinput_event *e;
        while ((e = libinput_get_event(li))) {
            enum libinput_event_type type = libinput_event_get_type(e);
            struct libinput_device *device = libinput_event_get_device(e);
            struct swipe *s = libinput_device_get_user_data(device);
            if (type == LIBINPUT_EVENT_DEVICE_ADDED) {
                s = calloc(1, sizeof *s);
                if (!s) { running = 0; result = 1; }
                libinput_device_set_user_data(device, s);
                printf("Touchpad connected: %s\n", libinput_device_get_name(device));
            } else if (type == LIBINPUT_EVENT_DEVICE_REMOVED) {
                free(s); libinput_device_set_user_data(device, NULL);
                puts("Touchpad disconnected");
            } else if (s && type == LIBINPUT_EVENT_POINTER_SCROLL_FINGER) {
                struct libinput_event_pointer *p = libinput_event_get_pointer_event(e);
                double x = libinput_event_pointer_get_scroll_value(p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
                double y = libinput_event_pointer_get_scroll_value(p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
                if (x == 0 && y == 0) {
                    if (s->active && s->fingers == 2) {
                        printf("Two-finger scroll end: x=%.1f y=%.1f path=%.1f/%.1f\n", s->x, s->y, s->travel, s->travel_y);
                        if (finish_back(s)) trigger(1, 2);
                    }
                } else {
                    if (!s->active || s->fingers != 2)
                        *s = (struct swipe){.active = true, .fingers = 2};
                    update(s, x, y);
                }
            } else if (s && type == LIBINPUT_EVENT_GESTURE_PINCH_BEGIN) {
                end_switch(true);
                *s = (struct swipe){0};
            } else if (s && (type == LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN ||
                       type == LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE || type == LIBINPUT_EVENT_GESTURE_SWIPE_END)) {
                struct libinput_event_gesture *g = libinput_event_get_gesture_event(e);
                if (type == LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN) {
                    int fingers = libinput_event_gesture_get_finger_count(g);
                    printf("Swipe begin: %d fingers\n", fingers);
                    *s = (struct swipe){.active = fingers == 3, .fingers = fingers};
                }
                else if (type == LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE) {
                    update(s, libinput_event_gesture_get_dx_unaccelerated(g), libinput_event_gesture_get_dy_unaccelerated(g));
                    if (s->active) live_switch(s);
                } else {
                    int fingers = s->fingers;
                    printf("Swipe end: %d fingers, x=%.1f y=%.1f path=%.1f/%.1f cancelled=%d\n", fingers, s->x, s->y, s->travel, s->travel_y, libinput_event_gesture_get_cancelled(g));
                    bool cancelled = libinput_event_gesture_get_cancelled(g);
                    bool switched = s->switching;
                    end_switch(cancelled);
                    int direction = finish(s, cancelled);
                    if (!switched && (direction == 2 || direction == -2)) trigger(direction, fingers);
                }
            }
            libinput_event_destroy(e);
        }
        if (!running) break;
        int ready = poll(&fd, 1, -1);
        if (ready < 0 && errno != EINTR) { perror("poll"); result = 1; break; }
        if (ready > 0 && (fd.revents & (POLLERR | POLLHUP | POLLNVAL))) { result = 1; break; }
    }
    end_switch(true);
    libinput_unref(li); udev_unref(u); close(lock);
    return result;
}
