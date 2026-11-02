/*
 * nwm - a small X11 window manager.
 *
 * Carries the Cinnamon tweaks from linux-essentials over to plain X:
 *
 *   Super+arrows        the windows-snap ladder: halves, quarters, max, min;
 *                       a half crosses to the next monitor before wrapping
 *   Super+Shift+Left/Right  move the window to the next monitor
 *   Alt+Tab             MRU switcher with small live thumbnails on the
 *                       pointer's monitor, shows at
 *                       once, Left/Right move along it too, Q closes the
 *                       highlighted window, Esc cancels
 *   Ctrl+Shift+W        close, except in terminals, which keep it for tabs
 *   Alt+F4, Super+Q     close
 *   Super+drag          move; dropped on the top edge maximizes, on a side
 *                       edge takes that half
 *   snap assist         a window snapped to an empty half offers the other
 *                       windows for the other half
 *   Super+right drag    resize from the nearest edge or corner
 *   bar                 a button per window (click focuses, or minimizes
 *                       the focused one), the root window name as status
 *                       text (xsetroot -name), tray icons, memory and swap
 *                       in use,
 *                       battery, a clock, a sound and a power button; hidden until the pointer touches the
 *                       bottom edge
 *   Super, tapped       launcher, and again to close it: type, Enter starts
 *                       the top match, or runs what was typed when nothing
 *                       matches; also has lock, log out, suspend, restart,
 *                       shut down
 *   window edges        drag just outside the focused window to resize
 *
 * There are no title bars. Apps with client-side title bars (Chrome, GTK
 * header bars) drag and resize through _NET_WM_MOVERESIZE as usual.
 *
 * At start it runs the XDG autostart entries for "nwm:X-Cinnamon", so the
 * same daemons and tray apps come up as under Cinnamon, then
 * ~/.config/nwm/autostart. ~/.config/nwm/displays runs before it, and
 * again whenever a screen is plugged in or out, to lay them out (xrandr).
 *
 * Build with make, try it nested with make run.
 */

#define _GNU_SOURCE	/* strcasestr */
#include <dirent.h>
#include <limits.h>
#include <locale.h>
#include <sys/select.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xft/Xft.h>

#define BORDER		0	/* px around windows, coloured by focus */
#define COL_FOCUS	"#5294e2"
#define COL_NORMAL	"#3a3a3a"
#define COL_TAB_BG	"#202020"
#define COL_TAB_SEL	"#2f5f9e"
#define COL_TAB_FG	"#e8e8e8"
#define COL_TAB_DIM	"#8a8a8a"
#define COL_TAB_BOX	"#2c2c2c"	/* behind a thumbnail */
#define FONT		"sans:size=11"
#define EDGE		2	/* px from a screen edge that count as on it */
#define CORNER		48	/* px along an edge that count as its corner */
#define GRIP		6	/* resize grip just outside the focused window */
#define GRIPCORNER	16	/* corner grips are this big, reaching inside */
#define DESKTOP		"nwm:X-Cinnamon"	/* XDG_CURRENT_DESKTOP default */
#define MINSIZE		64	/* smallest a window can be dragged down to */
#define MAXMONS		8
#define MAXDOCKS	24	/* other docks, plus each bar and its trigger */
#define MAXABOVE	16
#define MAXTAB		24
#define THUMBW		200	/* Alt+Tab thumbnail box */
#define THUMBH		125
#define MAXFALLBACK	8	/* fonts found for glyphs FONT lacks */
#define MAXAPPS		1024
#define LAUNCHROWS	10
#define TERMINAL	"gnome-terminal --"	/* for Terminal=true apps */
#define BARH		28	/* bottom bar on each monitor; 0 for none */
#define BARBTN		220	/* widest a window button gets */
#define BARGAP		7	/* between the things on the right */
#define AUTOHIDE	1	/* bar shows at the bottom edge; 0 keeps it up */
#define HIDEDELAY	400	/* ms after the pointer leaves it */
#define OSDTIME		600	/* ms the volume or brightness box stays up */
#define OSDW		300
#define OSDH		56
#define CLOCK		"%a %d %b  %H:%M"
#define POWER		"\u23fb"	/* the bar's power button */
#define SOUND		"\U0001f56a"	/* opens SOUNDCMD */
#define MUTEDICON	"\U0001f568"	/* speaker without waves */
#define LIGHTICON	"\u2600"
#define SOUNDCMD	"cinnamon-settings sound"
#define CHARGING	"\u26a1"	/* before the battery level, on AC */
#define PSU		"/sys/class/power_supply"
#define TRAYSZ		20	/* tray icon size, square */
#define TRAYPAD		5	/* empty edge inside a tray icon */
#define MAXTRAY		16

#define SUPER		Mod4Mask
#define ALT		Mod1Mask
#define LEN(a)		(sizeof(a) / sizeof((a)[0]))
#define min(a, b)	((a) < (b) ? (a) : (b))
#define max(a, b)	((a) > (b) ? (a) : (b))
#define MOUSEMASK	(ButtonPressMask | ButtonReleaseMask | PointerMotionMask)

/* Run these from the keyboard. Commands go through /bin/sh. */
#define ESS		"$HOME/github/linux-essentials"
/* Cinnamon's locker; its daemon is started from the autostart file */
#define LOCK		"cinnamon-screensaver-command -l"
/* Fn keys, which Cinnamon grabs for csd-media-keys */
#define VOL		"wpctl set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ "
#define MUTE		"wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle"
#define MICMUTE		"wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle"
/* brightness through logind, the backlight file being root's; never off */
#define LIGHT(d)	"b=$(brightnessctl g) m=$(brightnessctl m); " \
			"b=$((b " d " m / 20)); [ $b -lt $((m / 20)) ] && " \
			"b=$((m / 20)); [ $b -gt $m ] && b=$m; busctl call " \
			"org.freedesktop.login1 /org/freedesktop/login1/" \
			"session/auto org.freedesktop.login1.Session " \
			"SetBrightness ssu backlight " \
			"$(basename /sys/class/backlight/*) $b >/dev/null; " \
			"echo $((b * 100 / m))"
/* after VOL or MUTE: the level for the box, " m" after it while muted */
#define GETVOL		"; wpctl get-volume @DEFAULT_AUDIO_SINK@ | LC_ALL=C awk " \
			"'{ printf \"%d%s\\n\", $2 * 100 + 0.5, " \
			"/MUTED/ ? \" m\" : \"\" }'"

enum { K_SNAP, K_MOVEMON, K_CLOSE, K_TAB, K_SPAWN, K_OSD, K_QUIT };
enum { O_SOUND, O_LIGHT };	/* what K_OSD shows the level of */
enum { D_LEFT, D_RIGHT, D_UP, D_DOWN };

struct key {
	unsigned int mod;
	KeySym sym;
	int action;
	int arg;
	const char *cmd;
};

static const struct key keys[] = {
	{ SUPER,		XK_Left,	K_SNAP,		D_LEFT },
	{ SUPER,		XK_Right,	K_SNAP,		D_RIGHT },
	{ SUPER,		XK_Up,		K_SNAP,		D_UP },
	{ SUPER,		XK_Down,	K_SNAP,		D_DOWN },
	{ SUPER | ShiftMask,	XK_Left,	K_MOVEMON,	-1 },
	{ SUPER | ShiftMask,	XK_Right,	K_MOVEMON,	+1 },
	{ ALT,			XK_F4,		K_CLOSE },
	{ SUPER,		XK_q,		K_CLOSE },
	{ ALT,			XK_Tab,		K_TAB,		+1 },
	{ ALT | ShiftMask,	XK_Tab,		K_TAB,		-1 },
	{ ControlMask | ALT,	XK_t,		K_SPAWN, 0, "gnome-terminal" },
	{ SUPER,		XK_e,		K_SPAWN, 0, "nemo" },
	{ SUPER | ShiftMask,	XK_t,		K_SPAWN, 0, ESS "/ocr/ocrclip" },
	{ SUPER | ShiftMask,	XK_a,		K_SPAWN, 0, ESS "/ai/askclaude" },
	{ SUPER | ShiftMask,	XK_v,		K_SPAWN, 0, ESS "/voice/voicetoggle" },
	{ SUPER | ShiftMask,	XK_s,		K_SPAWN, 0, "gnome-screenshot -a -c" },
	{ SUPER,		XK_l,		K_SPAWN, 0, LOCK },
	{ ControlMask | ALT,	XK_l,		K_SPAWN, 0, LOCK },
	{ 0,	XF86XK_AudioRaiseVolume,	K_OSD, O_SOUND, VOL "5%+" GETVOL },
	{ 0,	XF86XK_AudioLowerVolume,	K_OSD, O_SOUND, VOL "5%-" GETVOL },
	{ 0,	XF86XK_AudioMute,		K_OSD, O_SOUND, MUTE GETVOL },
	{ 0,	XF86XK_AudioMicMute,		K_SPAWN, 0, MICMUTE },
	{ 0,	XF86XK_MonBrightnessUp,		K_OSD, O_LIGHT, LIGHT("+") },
	{ 0,	XF86XK_MonBrightnessDown,	K_OSD, O_LIGHT, LIGHT("-") },
	{ ControlMask | ALT,	XK_Delete,	K_QUIT },
};

/*
 * WM_CLASS names of apps that do something better with Ctrl+Shift+W than
 * close the window - terminals close a tab. While one of these has focus
 * the grab is released and the key reaches the app untouched.
 */
static const char *keepclose[] = {
	"gnome-terminal", "gnome-terminal-server", "xfce4-terminal",
	"konsole", "terminator", "alacritty", "kitty", "tilix",
	"x-terminal-emulator", NULL,
};

/*
 * Window states. Zones are in halves of the monitor work area: x, y, w, h.
 */
enum { FLOAT, MAX, TOP, LEFT, RIGHT, TL, TR, BL, BR, MINIMIZE, NOP };

static const struct { int x, y, w, h; } zones[] = {
	[MAX]	= { 0, 0, 2, 2 },
	[TOP]	= { 0, 0, 2, 1 },
	[LEFT]	= { 0, 0, 1, 2 },
	[RIGHT]	= { 1, 0, 1, 2 },
	[TL]	= { 0, 0, 1, 1 },
	[TR]	= { 1, 0, 1, 1 },
	[BL]	= { 0, 1, 1, 1 },
	[BR]	= { 1, 1, 1, 1 },
};

/*
 * The Super+arrow ladder, same as windows-snap@nybo. Halves cycle
 * left -> right -> floating. Quarters never float: a side arrow flips to
 * the other quarter in the row, the vertical arrow back toward the half
 * returns to it. Up climbs floating -> max -> top half and stops; down
 * drops any expanded window straight to floating, then minimizes.
 */
static const unsigned char ladder[][4] = {
	/*		left	right	up	down */
	[FLOAT]	= { LEFT,	RIGHT,	MAX,	MINIMIZE },
	[MAX]	= { LEFT,	RIGHT,	TOP,	FLOAT },
	[TOP]	= { LEFT,	RIGHT,	NOP,	FLOAT },
	[LEFT]	= { RIGHT,	FLOAT,	TL,	BL },
	[RIGHT]	= { FLOAT,	LEFT,	TR,	BR },
	[TL]	= { TR,		TR,	MAX,	LEFT },
	[TR]	= { TL,		TL,	MAX,	RIGHT },
	[BL]	= { BR,		BR,	LEFT,	NOP },
	[BR]	= { BL,		BL,	RIGHT,	NOP },
};

/* _NET_WM_MOVERESIZE directions */
enum { SZ_TL, SZ_T, SZ_TR, SZ_R, SZ_BR, SZ_B, SZ_BL, SZ_L, SZ_MOVE };

#define WM_ATOMS(X) \
	X(WM_PROTOCOLS) X(WM_DELETE_WINDOW) X(WM_STATE) X(WM_TAKE_FOCUS) \
	X(WM_CHANGE_STATE) X(UTF8_STRING) X(MANAGER)
/* underscored like the NET ones, but not EWMH, so not in _NET_SUPPORTED */
#define TRAY_ATOMS(X) \
	X(NET_SYSTEM_TRAY_OPCODE) X(NET_SYSTEM_TRAY_ORIENTATION) \
	X(NET_SYSTEM_TRAY_COLORS) \
	X(XEMBED) X(XEMBED_INFO)
#define NET_ATOMS(X) \
	X(NET_SUPPORTED) X(NET_SUPPORTING_WM_CHECK) X(NET_WM_NAME) \
	X(NET_ACTIVE_WINDOW) X(NET_CLIENT_LIST) X(NET_CLOSE_WINDOW) \
	X(NET_WM_MOVERESIZE) X(NET_WM_STATE) X(NET_WM_STATE_FULLSCREEN) \
	X(NET_WM_STATE_MAXIMIZED_VERT) X(NET_WM_STATE_MAXIMIZED_HORZ) \
	X(NET_WM_STATE_HIDDEN) X(NET_WM_WINDOW_TYPE) \
	X(NET_WM_WINDOW_TYPE_DOCK) X(NET_WM_WINDOW_TYPE_DESKTOP) \
	X(NET_WM_WINDOW_TYPE_NOTIFICATION) X(NET_WM_WINDOW_TYPE_TOOLTIP) \
	X(NET_WM_WINDOW_TYPE_POPUP_MENU) X(NET_WM_WINDOW_TYPE_DROPDOWN_MENU) \
	X(NET_WM_WINDOW_TYPE_COMBO) X(NET_WM_WINDOW_TYPE_DND) \
	X(NET_WM_STRUT) X(NET_WM_STRUT_PARTIAL) X(NET_WORKAREA) \
	X(NET_FRAME_EXTENTS) X(NET_WM_DESKTOP) X(NET_NUMBER_OF_DESKTOPS) \
	X(NET_CURRENT_DESKTOP)
#define ENUM(a)		a,
#define WMNAME(a)	#a,
#define NETNAME(a)	"_" #a,

enum { WM_ATOMS(ENUM) TRAY_ATOMS(ENUM) NET_ATOMS(ENUM) NATOMS };
static char *atomname[] = {
	WM_ATOMS(WMNAME) TRAY_ATOMS(NETNAME) NET_ATOMS(NETNAME)
};

struct rect {
	int x, y, w, h;
};

struct client {
	Window win;
	Window transfor;
	int x, y, w, h;		/* outer geometry, border included */
	int bw;
	struct rect fl;		/* where the window floats back to */
	struct rect home;	/* fl when the screen was homew x homeh */
	int state;
	int minimized, fullscreen, fixed, noinput;
	int ignoreunmap;	/* unmaps we did ourselves, to minimize */
	int redirected;		/* contents kept offscreen, for thumbnails */
	Pixmap thumb;		/* last thumbnail, kept for when minimized */
	Picture thumbpic;
	int hasthumb;
	unsigned long seq;	/* order of mapping, for the bar */
	struct client *next;	/* most recently focused first */
};

struct dock {
	Window win;
	long strut[12];
};

static Display *dpy;
static Window root, checkwin, tabwin, osdwin;
static int scr, sw, sh, otherwm;
static int rrbase = -1;			/* XRandR event base */
static volatile sig_atomic_t running = 1, restarting;
static Atom atom[NATOMS];
static unsigned long pix_focus, pix_normal;
static Cursor cur_normal, cur_move, cur_resize;
static Window grips[8];		/* indexed by SZ_ direction */
static unsigned int numlockmask;
static struct rect mons[MAXMONS];
static int nmons;
static int homew, homeh;	/* screen size floating windows go back to */
static int scanning;		/* adopting the windows already up */
static struct dock docks[MAXDOCKS];
static int ndocks;
/* notifications and the like: mapped as asked, kept on top, not managed */
static Window above[MAXABOVE];
static int nabove;
static struct client *clients, *sel;
static int closegrabbed;
static unsigned long nextseq;

/* Bars, one per monitor; the first one owns the tray */
static struct bar {
	Window win;
	Window trigger;		/* 1px along the bottom edge, while hidden */
	int shown;
	XftDraw *draw;
	struct {
		int x, w;
		struct client *c;
	} btn[MAXTAB];
	int nbtn;
	int powerx;		/* where the power button starts */
	int soundx, soundend;	/* the sound button, on the first bar */
} bars[MAXMONS];
static int nbars, bardirty;
static long long hideat;	/* ms, monotonic; 0 when no hide is due */
#define TRAYWIN		(bars[0].win)

/* System tray: XEmbed icons living in the bar window */
static Atom traysel;
static struct {
	Window win;
	int x, mapped;
} tray[MAXTRAY];
static int ntray;

/* Alt+Tab */
static struct client *tab[MAXTAB];
static int ntab, tabsel, tabbing;
static int tabassist, assistzone, assistmon;	/* the cards as snap assist */
static struct rect cards[MAXTAB];		/* where drawtab put them */
static Pixmap assistbg;				/* the half, dimmed, behind them */
static Picture assistbgpic;
static int tabx, taby;
static XftFont *font, *fallback[MAXFALLBACK];
static int nfallback;
static FcChar32 nofont[32];	/* codepoints no font has, to stop looking */
static int nnofont;
static XftDraw *tabdraw;

/* Volume and brightness box: the level comes back from a K_OSD command */
static XftDraw *osddraw;
static int osdfd = -1, osdwhat;
static char osdbuf[64];
static size_t osdlen;
static long long osdhideat;	/* ms, monotonic; 0 when it is not up */
static XftColor xc_bg, xc_sel, xc_fg, xc_dim, xc_box;
static int composite;		/* Composite and Render are there */
static Picture tabpic;
static XRenderPictFormat *rootfmt;

/* Launcher, in the same popup window as Alt+Tab */
struct app {
	char id[128];		/* desktop file name; the first one wins */
	char name[128];
	char exec[512];
	int hidden;		/* not an app at all, or deleted */
	int nodisplay;		/* an app, but not for menus */
	int session;		/* one of our own lock, log out ... entries */
	int elsewhere;		/* not for this desktop, or switched off */
};

static struct app *apps;
static int napps, launching, launchsession, superalone, noauto;
/* snapped to a half by Super+arrow: assist waits for Super to go up */
static struct client *assistpend;
static int match[LAUNCHROWS], nmatch, launchsel;
static char query[128];
static size_t qlen;

static void handle(XEvent *ev);
static void focus(struct client *c);
static void drawtab(void);
static void movemon(struct client *c, int dir);
static void osdrun(const char *cmd, int what);
static void endtab(struct client *c);
static void placegrips(void);
static int updatethumb(struct client *c);
static void launchstart(int session);
static void assiststart(struct client *c);

static void die(const char *msg)
{
	fputs(msg, stderr);
	exit(1);
}

static void spawn(const char *cmd)
{
	struct sigaction sa = { .sa_handler = SIG_DFL };

	if (fork() != 0)
		return;
	if (dpy)
		close(ConnectionNumber(dpy));
	setsid();
	sigemptyset(&sa.sa_mask);
	sigaction(SIGCHLD, &sa, NULL);
	execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
	_exit(127);
}

static void displays(void)
{
	spawn("f=\"${XDG_CONFIG_HOME:-$HOME/.config}/nwm/displays\"; "
	      "[ -x \"$f\" ] && exec \"$f\"");
}

static unsigned int cleanmask(unsigned int mask)
{
	return mask & ~(numlockmask | LockMask) &
		(ShiftMask | ControlMask | Mod1Mask | Mod2Mask | Mod3Mask |
		 Mod4Mask | Mod5Mask);
}

static long *getlongs(Window w, Atom prop, Atom type, unsigned long *n)
{
	Atom real;
	int fmt;
	unsigned long after;
	unsigned char *p = NULL;

	if (XGetWindowProperty(dpy, w, prop, 0, 64, False, type, &real, &fmt,
			       n, &after, &p) != Success || real != type ||
	    fmt != 32 || !*n) {
		if (p)
			XFree(p);
		return NULL;
	}
	return (long *)p;
}

static int hasatom(Window w, int prop, int value)
{
	unsigned long i, n;
	long *v = getlongs(w, atom[prop], XA_ATOM, &n);
	int hit = 0;

	for (i = 0; v && i < n; i++)
		hit |= (Atom)v[i] == atom[value];
	if (v)
		XFree(v);
	return hit;
}

/* Monitors */

static void updatemons(void)
{
	XineramaScreenInfo *info;
	XWindowAttributes wa;
	struct rect r;
	int i, j, n = 0;

	XGetWindowAttributes(dpy, root, &wa);
	sw = wa.width;
	sh = wa.height;
	nmons = 0;
	if (XineramaIsActive(dpy) && (info = XineramaQueryScreens(dpy, &n))) {
		for (i = 0; i < n && nmons < MAXMONS; i++) {
			r = (struct rect){ info[i].x_org, info[i].y_org,
					   info[i].width, info[i].height };
			/* mirrored outputs show up twice */
			for (j = 0; j < nmons; j++)
				if (!memcmp(&mons[j], &r, sizeof(r)))
					break;
			if (j < nmons)
				continue;
			/* left to right, so Super+Shift+Right goes right */
			for (j = nmons; j > 0 && (mons[j - 1].x > r.x ||
			     (mons[j - 1].x == r.x && mons[j - 1].y > r.y)); j--)
				mons[j] = mons[j - 1];
			mons[j] = r;
			nmons++;
		}
		XFree(info);
	}
	if (!nmons) {
		mons[0] = (struct rect){ 0, 0, sw, sh };
		nmons = 1;
	}
}

static int monat(int x, int y)
{
	int i;

	for (i = 0; i < nmons; i++)
		if (x >= mons[i].x && x < mons[i].x + mons[i].w &&
		    y >= mons[i].y && y < mons[i].y + mons[i].h)
			return i;
	return -1;
}

static int nearmon(int x, int y)
{
	long d, dx, dy, bestd = LONG_MAX;
	int i, best = 0;

	for (i = 0; i < nmons; i++) {
		struct rect *r = &mons[i];

		dx = x < r->x ? r->x - x :
		     x >= r->x + r->w ? x - (r->x + r->w - 1) : 0;
		dy = y < r->y ? r->y - y :
		     y >= r->y + r->h ? y - (r->y + r->h - 1) : 0;
		d = dx * dx + dy * dy;
		if (d < bestd) {
			bestd = d;
			best = i;
		}
	}
	return best;
}

static int clientmon(struct client *c)
{
	return nearmon(c->x + c->w / 2, c->y + c->h / 2);
}

static int pointermon(void)
{
	Window dw;
	int x, y, di;
	unsigned int mask;

	XQueryPointer(dpy, root, &dw, &dw, &x, &y, &di, &di, &mask);
	return nearmon(x, y);
}

/* The monitor minus whatever docks reserve along the screen edges. */
static struct rect workarea(int m)
{
	struct rect r = mons[m];
	int i, x1 = r.x, y1 = r.y, x2 = r.x + r.w, y2 = r.y + r.h;

	for (i = 0; i < ndocks; i++) {
		long *s = docks[i].strut;

		/* left, right, top, bottom, then start/end pairs for each */
		if (s[0] > x1 && s[0] < x2 && s[5] >= y1 && s[4] < y2)
			x1 = s[0];
		if (s[1] && sw - s[1] < x2 && sw - s[1] > x1 &&
		    s[7] >= y1 && s[6] < y2)
			x2 = sw - s[1];
		if (s[2] > y1 && s[2] < y2 && s[9] >= x1 && s[8] < x2)
			y1 = s[2];
		if (s[3] && sh - s[3] < y2 && sh - s[3] > y1 &&
		    s[11] >= x1 && s[10] < x2)
			y2 = sh - s[3];
	}
	return (struct rect){ x1, y1, x2 - x1, y2 - y1 };
}

static void updateworkarea(void)
{
	struct rect a = workarea(0);
	long v[4] = { a.x, a.y, a.w, a.h };

	XChangeProperty(dpy, root, atom[NET_WORKAREA], XA_CARDINAL, 32,
			PropModeReplace, (unsigned char *)v, 4);
}

/* Docks */

static struct dock *finddock(Window w)
{
	int i;

	for (i = 0; i < ndocks; i++)
		if (docks[i].win == w)
			return &docks[i];
	return NULL;
}

static void readstrut(struct dock *d)
{
	unsigned long n;
	long *v;

	memset(d->strut, 0, sizeof(d->strut));
	if ((v = getlongs(d->win, atom[NET_WM_STRUT_PARTIAL], XA_CARDINAL,
			  &n)) && n >= 12) {
		memcpy(d->strut, v, sizeof(d->strut));
	} else {
		if (v)
			XFree(v);
		v = getlongs(d->win, atom[NET_WM_STRUT], XA_CARDINAL, &n);
		if (v && n >= 4) {
			memcpy(d->strut, v, 4 * sizeof(long));
			d->strut[5] = d->strut[7] = sh;
			d->strut[9] = d->strut[11] = sw;
		}
	}
	if (v)
		XFree(v);
}

static int removedock(Window w)
{
	struct dock *d = finddock(w);

	if (!d)
		return 0;
	*d = docks[--ndocks];
	return 1;
}

/* Clients */

static struct client *find(Window w)
{
	struct client *c;

	for (c = clients; c; c = c->next)
		if (c->win == w)
			return c;
	return NULL;
}

static int sendproto(struct client *c, Atom proto, Time t)
{
	XEvent ev = { 0 };
	Atom *p;
	int n, found = 0;

	if (XGetWMProtocols(dpy, c->win, &p, &n)) {
		while (n--)
			found |= p[n] == proto;
		XFree(p);
	}
	if (!found)
		return 0;
	ev.type = ClientMessage;
	ev.xclient.window = c->win;
	ev.xclient.message_type = atom[WM_PROTOCOLS];
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = proto;
	ev.xclient.data.l[1] = t;
	XSendEvent(dpy, c->win, False, NoEventMask, &ev);
	return 1;
}

/* The polite close, same as clicking the X: the app may still ask first. */
static void closewin(struct client *c)
{
	if (!sendproto(c, atom[WM_DELETE_WINDOW], CurrentTime))
		XKillClient(dpy, c->win);
}

static void setwmstate(struct client *c, long state)
{
	long v[2] = { state, None };

	XChangeProperty(dpy, c->win, atom[WM_STATE], atom[WM_STATE], 32,
			PropModeReplace, (unsigned char *)v, 2);
}

static void setnetstate(struct client *c)
{
	Atom a[4];
	int n = 0;

	if (c->fullscreen)
		a[n++] = atom[NET_WM_STATE_FULLSCREEN];
	if (c->state == MAX) {
		a[n++] = atom[NET_WM_STATE_MAXIMIZED_VERT];
		a[n++] = atom[NET_WM_STATE_MAXIMIZED_HORZ];
	}
	if (c->minimized)
		a[n++] = atom[NET_WM_STATE_HIDDEN];
	XChangeProperty(dpy, c->win, atom[NET_WM_STATE], XA_ATOM, 32,
			PropModeReplace, (unsigned char *)a, n);
}

static void updateclientlist(void)
{
	struct client *c;

	XDeleteProperty(dpy, root, atom[NET_CLIENT_LIST]);
	for (c = clients; c; c = c->next)
		XChangeProperty(dpy, root, atom[NET_CLIENT_LIST], XA_WINDOW,
				32, PropModeAppend, (unsigned char *)&c->win, 1);
}

static void sendconfig(struct client *c)
{
	XConfigureEvent ce = {
		.type = ConfigureNotify,
		.display = dpy,
		.event = c->win,
		.window = c->win,
		.x = c->x,
		.y = c->y,
		.width = c->w - 2 * c->bw,
		.height = c->h - 2 * c->bw,
		.border_width = c->bw,
		.above = None,
		.override_redirect = False,
	};

	XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

/*
 * Place the window. While it floats, wherever it goes is also where it
 * floats back to, so the floating geometry never needs saving separately.
 */
static void resize(struct client *c, int x, int y, int w, int h)
{
	w = max(w, 2 * c->bw + 1);
	h = max(h, 2 * c->bw + 1);
	c->x = x;
	c->y = y;
	c->w = w;
	c->h = h;
	if (c->state == FLOAT && !c->fullscreen)
		c->fl = (struct rect){ x, y, w, h };
	XMoveResizeWindow(dpy, c->win, x, y, w - 2 * c->bw, h - 2 * c->bw);
	if (c == sel)
		placegrips();
}

/*
 * A window that opened screen-sized (a browser restoring its last size,
 * say) has nothing useful to float back to, so it gets a centred box at
 * 60% instead, small enough to clearly read as un-snapped. Either way it
 * comes back inside the work area: it may have been dropped on an edge
 * half off screen.
 */
static struct rect floatrect(struct client *c)
{
	struct rect a = workarea(clientmon(c)), f = c->fl;

	if (f.w >= a.w * 9 / 10 && f.h >= a.h * 9 / 10) {
		f.w = a.w * 6 / 10;
		f.h = a.h * 6 / 10;
		f.x = a.x + (a.w - f.w) / 2;
		f.y = a.y + (a.h - f.h) / 2;
	}
	f.x = max(a.x, min(f.x, a.x + a.w - f.w));
	f.y = max(a.y, min(f.y, a.y + a.h - f.h));
	return f;
}

static struct rect zonerect(int state, int m)
{
	struct rect a = workarea(m);
	int x1 = a.x + a.w * zones[state].x / 2;
	int y1 = a.y + a.h * zones[state].y / 2;
	int x2 = a.x + a.w * (zones[state].x + zones[state].w) / 2;
	int y2 = a.y + a.h * (zones[state].y + zones[state].h) / 2;

	return (struct rect){ x1, y1, x2 - x1, y2 - y1 };
}

/* Put the window where its state says, on monitor m. */
static void arrange(struct client *c, int m)
{
	struct rect r;

	if (c->fullscreen)
		r = mons[m];
	else if (c->state == FLOAT)
		r = floatrect(c);
	else
		r = zonerect(c->state, m);
	resize(c, r.x, r.y, r.w, r.h);
	setnetstate(c);
}

/*
 * The screen changed size (rotated, or a phone's remote session taking it
 * over): floating windows are about to be pulled inside it, so where they
 * were is kept, and given back when the screen is that size again.
 */
static void keephome(int ow, int oh)
{
	struct client *c;

	if (sw == ow && sh == oh)
		return;
	if (homew && sw == homew && sh == homeh) {
		for (c = clients; c; c = c->next)
			if (c->home.w)
				c->fl = c->home;
		homew = homeh = 0;
	} else if (!homew) {
		for (c = clients; c; c = c->next)
			c->home = c->fl;
		homew = ow;
		homeh = oh;
	}
}

/*
 * Monitors or docks changed: zones follow, and floating windows are pulled
 * back inside the work area, since a rotated or unplugged screen can leave
 * them half off it.
 */
static void relayout(void)
{
	struct client *c;

	for (c = clients; c; c = c->next)
		arrange(c, clientmon(c));
	updateworkarea();
}

/*
 * Automatic redirection keeps a copy of the window's contents offscreen,
 * which is where thumbnails come from, while the server still draws it
 * on screen as usual. Fullscreen windows skip it: a game should go
 * straight to the screen.
 */
static void redirect(struct client *c, int on)
{
	if (!composite || on == c->redirected)
		return;
	if (on)
		XCompositeRedirectWindow(dpy, c->win, CompositeRedirectAutomatic);
	else
		XCompositeUnredirectWindow(dpy, c->win,
					   CompositeRedirectAutomatic);
	c->redirected = on;
}

static void setfullscreen(struct client *c, int on)
{
	int m = clientmon(c);

	redirect(c, !on);
	c->fullscreen = on;
	c->bw = on ? 0 : BORDER;
	XSetWindowBorderWidth(dpy, c->win, c->bw);
	arrange(c, m);
	if (on)
		XRaiseWindow(dpy, c->win);
}

static void setmax(struct client *c, int on)
{
	if (c->fixed || on == (c->state == MAX))
		return;
	c->state = on ? MAX : FLOAT;
	arrange(c, clientmon(c));
}

static void restack(struct client *c)
{
	struct client *t;
	int i;

	XRaiseWindow(dpy, c->win);
	/* its dialogs stay above it */
	for (t = clients; t; t = t->next)
		if (t->transfor == c->win && !t->minimized)
			XRaiseWindow(dpy, t->win);
	placegrips();
	if (!c->fullscreen)
		for (i = 0; i < ndocks; i++)
			XRaiseWindow(dpy, docks[i].win);
	for (i = 0; i < nabove; i++)
		XRaiseWindow(dpy, above[i]);
	if (tabbing)
		XRaiseWindow(dpy, tabwin);
}

static void minimize(struct client *c)
{
	struct client **pp;

	if (c->minimized)
		return;
	/* unmapped, it has no contents: keep what it looked like */
	updatethumb(c);
	c->minimized = 1;
	c->ignoreunmap++;
	XUnmapWindow(dpy, c->win);
	setwmstate(c, IconicState);
	setnetstate(c);
	bardirty = 1;

	/* to the back of the MRU list, so Alt+Tab goes to what is showing */
	for (pp = &clients; *pp != c; pp = &(*pp)->next)
		;
	*pp = c->next;
	for (; *pp; pp = &(*pp)->next)
		;
	*pp = c;
	c->next = NULL;

	if (c == sel)
		focus(NULL);
}

static void snap(struct client *c, int dir)
{
	int next;

	if (c->fixed || c->fullscreen)
		return;
	/* a half pushed against a neighbouring monitor crosses onto its
	 * near half; only at the outer edge does it wrap as the ladder says */
	if ((c->state == LEFT && dir == D_LEFT && clientmon(c) > 0) ||
	    (c->state == RIGHT && dir == D_RIGHT &&
	     clientmon(c) < nmons - 1)) {
		c->state = dir == D_LEFT ? RIGHT : LEFT;
		movemon(c, dir == D_LEFT ? -1 : +1);
		assistpend = c;
		return;
	}
	next = ladder[c->state][dir];
	if (next == NOP)
		return;
	if (next == MINIMIZE) {
		minimize(c);
		return;
	}
	c->state = next;
	arrange(c, clientmon(c));
	assistpend = next == LEFT || next == RIGHT ? c : NULL;
}

static void movemon(struct client *c, int dir)
{
	int m = clientmon(c), n;
	struct rect a, b;

	if (nmons < 2)
		return;
	n = (m + dir + nmons) % nmons;
	a = workarea(m);
	b = workarea(n);
	/* floating geometry keeps its offset into the work area */
	c->fl.x = b.x + min(max(c->fl.x - a.x, 0), max(b.w - c->fl.w, 0));
	c->fl.y = b.y + min(max(c->fl.y - a.y, 0), max(b.h - c->fl.h, 0));
	c->x += b.x - a.x;
	c->y += b.y - a.y;
	arrange(c, n);
}

static void grabbuttons(struct client *c, int focused)
{
	unsigned int extra[] = { 0, LockMask, numlockmask,
				 LockMask | numlockmask };
	size_t i;

	XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
	/* any click on an unfocused window focuses it, then goes through */
	if (!focused)
		XGrabButton(dpy, AnyButton, AnyModifier, c->win, False,
			    ButtonPressMask, GrabModeSync, GrabModeAsync,
			    None, None);
	for (i = 0; i < LEN(extra); i++) {
		XGrabButton(dpy, Button1, SUPER | extra[i], c->win, False,
			    ButtonPressMask, GrabModeAsync, GrabModeAsync,
			    None, None);
		XGrabButton(dpy, Button3, SUPER | extra[i], c->win, False,
			    ButtonPressMask, GrabModeAsync, GrabModeAsync,
			    None, None);
	}
}

static void grabkey(unsigned int mod, KeySym sym, int on)
{
	unsigned int extra[] = { 0, LockMask, numlockmask,
				 LockMask | numlockmask };
	KeyCode code = XKeysymToKeycode(dpy, sym);
	size_t i;

	if (!code)
		return;
	for (i = 0; i < LEN(extra); i++) {
		if (on)
			XGrabKey(dpy, code, mod | extra[i], root, True,
				 GrabModeAsync, GrabModeAsync);
		else
			XUngrabKey(dpy, code, mod | extra[i], root);
	}
}

static int keepsclose(struct client *c)
{
	XClassHint ch = { 0 };
	int i, hit = 0;

	if (!c || !XGetClassHint(dpy, c->win, &ch))
		return 0;
	for (i = 0; keepclose[i]; i++)
		hit |= (ch.res_name && !strcasecmp(ch.res_name, keepclose[i])) ||
		       (ch.res_class && !strcasecmp(ch.res_class, keepclose[i]));
	if (ch.res_name)
		XFree(ch.res_name);
	if (ch.res_class)
		XFree(ch.res_class);
	return hit;
}

/*
 * Ctrl+Shift+W is only grabbed while the focused app has no use for it, the
 * same trick as smart-close@nybo: no key re-injection, the terminal just
 * gets the real key when it has focus.
 */
static void updateclosegrab(void)
{
	int want = !keepsclose(sel);

	if (want == closegrabbed)
		return;
	grabkey(ControlMask | ShiftMask, XK_w, want);
	closegrabbed = want;
}

static void updatenumlock(void)
{
	XModifierKeymap *mm = XGetModifierMapping(dpy);
	KeyCode nl = XKeysymToKeycode(dpy, XK_Num_Lock);
	int i, j;

	numlockmask = 0;
	for (i = 0; i < 8; i++)
		for (j = 0; j < mm->max_keypermod; j++)
			if (nl && mm->modifiermap[i * mm->max_keypermod + j] == nl)
				numlockmask = 1 << i;
	XFreeModifiermap(mm);
}

static int isaltkey(KeyCode code)
{
	XModifierKeymap *mm = XGetModifierMapping(dpy);
	int j, hit = 0;

	for (j = 0; j < mm->max_keypermod; j++)
		hit |= mm->modifiermap[Mod1MapIndex * mm->max_keypermod + j] ==
		       code;
	XFreeModifiermap(mm);
	return hit;
}

static void grabkeys(void)
{
	size_t i;

	updatenumlock();
	XUngrabKey(dpy, AnyKey, AnyModifier, root);
	for (i = 0; i < LEN(keys); i++)
		grabkey(keys[i].mod, keys[i].sym, 1);
	grabkey(0, XK_Super_L, 1);
	grabkey(0, XK_Super_R, 1);
	closegrabbed = 0;
	updateclosegrab();
}

/*
 * The server's time now, from a property change on checkwin. Focus set at
 * CurrentTime always wins, so a busy app that gets to an old WM_TAKE_FOCUS
 * late would take the keyboard back from the window focused since; with
 * real times the server drops its stale request.
 */
static Time servertime(void)
{
	XEvent ev;

	if (!checkwin)
		return CurrentTime;
	XChangeProperty(dpy, checkwin, atom[NET_WM_NAME], atom[UTF8_STRING], 8,
			PropModeAppend, NULL, 0);
	XWindowEvent(dpy, checkwin, PropertyChangeMask, &ev);
	return ev.xproperty.time;
}

static void focus(struct client *c)
{
	Time t;

	struct client **pp;

	if (!c)
		for (c = clients; c && c->minimized; c = c->next)
			;
	if (sel && sel != c) {
		XSetWindowBorder(dpy, sel->win, pix_normal);
		grabbuttons(sel, 0);
	}
	sel = c;
	if (!c) {
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, atom[NET_ACTIVE_WINDOW]);
		placegrips();
		updateclosegrab();
		bardirty = 1;
		return;
	}
	if (c->minimized) {
		c->minimized = 0;
		XMapWindow(dpy, c->win);
		setwmstate(c, NormalState);
		setnetstate(c);
	}
	/* to the front of the MRU list */
	for (pp = &clients; *pp != c; pp = &(*pp)->next)
		;
	*pp = c->next;
	c->next = clients;
	clients = c;

	XSetWindowBorder(dpy, c->win, pix_focus);
	grabbuttons(c, 1);
	restack(c);
	t = servertime();
	if (!c->noinput)
		XSetInputFocus(dpy, c->win, RevertToPointerRoot, t);
	sendproto(c, atom[WM_TAKE_FOCUS], t);
	XChangeProperty(dpy, root, atom[NET_ACTIVE_WINDOW], XA_WINDOW, 32,
			PropModeReplace, (unsigned char *)&c->win, 1);
	updateclosegrab();
	bardirty = 1;
}

/* Re-read the input hint; a selected window that turns it on gets the keys. */
static void updatenoinput(struct client *c)
{
	XWMHints *wmh = XGetWMHints(dpy, c->win);
	int was = c->noinput;

	c->noinput = wmh && (wmh->flags & InputHint) && !wmh->input;
	if (wmh)
		XFree(wmh);
	if (c == sel && was && !c->noinput)
		XSetInputFocus(dpy, c->win, RevertToPointerRoot, servertime());
}

static void manage(Window w, XWindowAttributes *wa)
{
	struct client *c = calloc(1, sizeof(*c)), *t;
	XSizeHints hints;
	struct rect a;
	long supplied, ext[4];
	int m;

	if (!c)
		return;
	c->win = w;
	c->seq = nextseq++;
	c->fullscreen = hasatom(w, NET_WM_STATE, NET_WM_STATE_FULLSCREEN);
	c->bw = c->fullscreen ? 0 : BORDER;
	c->x = wa->x;
	c->y = wa->y;
	c->w = wa->width + 2 * c->bw;
	c->h = wa->height + 2 * c->bw;
	XGetTransientForHint(dpy, w, &c->transfor);
	if (!XGetWMNormalHints(dpy, w, &hints, &supplied))
		hints.flags = 0;
	c->fixed = (hints.flags & PMinSize) && (hints.flags & PMaxSize) &&
		   hints.min_width == hints.max_width &&
		   hints.min_height == hints.max_height;
	updatenoinput(c);

	/*
	 * Dialogs centre over their parent. Anything that did not ask for a
	 * position centres on the monitor the pointer is on, unless it was
	 * already up (a restart): it stays where it is.
	 */
	if (scanning) {
		m = clientmon(c);
	} else if ((t = find(c->transfor))) {
		c->x = t->x + (t->w - c->w) / 2;
		c->y = t->y + (t->h - c->h) / 2;
		m = clientmon(t);
	} else if (!(hints.flags & (USPosition | PPosition)) ||
		   (!c->x && !c->y)) {
		m = pointermon();
		a = workarea(m);
		c->x = a.x + (a.w - c->w) / 2;
		c->y = a.y + (a.h - c->h) / 2;
	} else {
		m = clientmon(c);
	}
	a = workarea(m);
	c->w = min(c->w, a.w);
	c->h = min(c->h, a.h);
	c->x = max(a.x, min(c->x, a.x + a.w - c->w));
	c->y = max(a.y, min(c->y, a.y + a.h - c->h));
	c->fl = (struct rect){ c->x, c->y, c->w, c->h };
	c->state = FLOAT;
	if (!c->fixed && hasatom(w, NET_WM_STATE, NET_WM_STATE_MAXIMIZED_VERT) &&
	    hasatom(w, NET_WM_STATE, NET_WM_STATE_MAXIMIZED_HORZ))
		c->state = MAX;

	XSetWindowBorderWidth(dpy, w, c->bw);
	XSetWindowBorder(dpy, w, pix_normal);
	XSelectInput(dpy, w, PropertyChangeMask);
	grabbuttons(c, 0);
	ext[0] = ext[1] = ext[2] = ext[3] = BORDER;
	XChangeProperty(dpy, w, atom[NET_FRAME_EXTENTS], XA_CARDINAL, 32,
			PropModeReplace, (unsigned char *)ext, 4);
	ext[0] = 0;
	XChangeProperty(dpy, w, atom[NET_WM_DESKTOP], XA_CARDINAL, 32,
			PropModeReplace, (unsigned char *)ext, 1);

	c->next = clients;
	clients = c;
	redirect(c, !c->fullscreen);
	arrange(c, m);
	updateclientlist();
	XMapWindow(dpy, w);
	setwmstate(c, NormalState);
	focus(c);
}

static void unmanage(struct client *c, int destroyed)
{
	struct client **pp;
	int i;

	for (pp = &clients; *pp != c; pp = &(*pp)->next)
		;
	*pp = c->next;
	if (assistpend == c)
		assistpend = NULL;

	for (i = 0; i < ntab && tab[i] != c; i++)
		;
	if (i < ntab) {
		memmove(&tab[i], &tab[i + 1], (ntab - i - 1) * sizeof(*tab));
		ntab--;
		if (tabsel > i || tabsel >= ntab)
			tabsel = max(tabsel - 1, 0);
	}

	if (c->thumb) {
		XRenderFreePicture(dpy, c->thumbpic);
		XFreePixmap(dpy, c->thumb);
	}
	if (!destroyed) {
		XGrabServer(dpy);
		redirect(c, 0);
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		XSelectInput(dpy, c->win, NoEventMask);
		setwmstate(c, WithdrawnState);
		XSync(dpy, False);
		XUngrabServer(dpy);
	}
	if (sel == c) {
		sel = NULL;
		focus(NULL);
	}
	free(c);
	updateclientlist();
	bardirty = 1;
	if (tabbing) {
		if (ntab)
			drawtab();
		else
			endtab(NULL);
	}
}

static int isabovetype(Window w)
{
	static const int types[] = {
		NET_WM_WINDOW_TYPE_NOTIFICATION, NET_WM_WINDOW_TYPE_TOOLTIP,
		NET_WM_WINDOW_TYPE_POPUP_MENU, NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
		NET_WM_WINDOW_TYPE_COMBO, NET_WM_WINDOW_TYPE_DND,
	};
	size_t i;

	for (i = 0; i < LEN(types); i++)
		if (hasatom(w, NET_WM_WINDOW_TYPE, types[i]))
			return 1;
	return 0;
}

static int removeabove(Window w)
{
	int i;

	for (i = 0; i < nabove; i++)
		if (above[i] == w) {
			above[i] = above[--nabove];
			return 1;
		}
	return 0;
}

/* Docks and desktops are mapped but never managed. */
static void adopt(Window w, XWindowAttributes *wa)
{
	struct dock *d;

	if (hasatom(w, NET_WM_WINDOW_TYPE, NET_WM_WINDOW_TYPE_DOCK)) {
		if (!finddock(w) && ndocks < MAXDOCKS) {
			d = &docks[ndocks++];
			d->win = w;
			readstrut(d);
			XSelectInput(dpy, w, PropertyChangeMask);
			relayout();
		}
		XMapRaised(dpy, w);
		return;
	}
	if (hasatom(w, NET_WM_WINDOW_TYPE, NET_WM_WINDOW_TYPE_DESKTOP)) {
		XMapWindow(dpy, w);
		XLowerWindow(dpy, w);
		return;
	}
	/*
	 * Notification bubbles, tooltips and menus that are not
	 * override-redirect: they place themselves, and should neither take
	 * focus nor show up in the bar.
	 */
	if (isabovetype(w)) {
		removeabove(w);
		if (nabove < MAXABOVE)
			above[nabove++] = w;
		XMapRaised(dpy, w);
		return;
	}
	manage(w, wa);
}

/* Mouse */

static int buttonheld(void)
{
	Window dw;
	int di;
	unsigned int mask;

	XQueryPointer(dpy, root, &dw, &dw, &di, &di, &di, &di, &mask);
	return mask & (Button1Mask | Button2Mask | Button3Mask);
}

/*
 * Grab the pointer for a drag. A client asking for one with
 * _NET_WM_MOVERESIZE may already have seen the button go up, and then
 * there is nothing to drag.
 */
static int grabdrag(Cursor cur)
{
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync,
			 GrabModeAsync, None, cur, CurrentTime) != GrabSuccess)
		return 0;
	if (buttonheld())
		return 1;
	XUngrabPointer(dpy, CurrentTime);
	return 0;
}

/* Wait for the next drag event, serving the rest in the meantime. */
static void nextdrag(XEvent *ev)
{
	for (;;) {
		XMaskEvent(dpy, MOUSEMASK | ExposureMask |
			   SubstructureRedirectMask, ev);
		if (ev->type == MotionNotify || ev->type == ButtonRelease)
			return;
		if (ev->type != ButtonPress)
			handle(ev);
	}
}

/*
 * Dropping a window with the pointer against a screen edge snaps it: the
 * top edge maximizes, a side takes that half, and near a corner it takes
 * that quarter. Only real edges count, not the seam between two monitors.
 */
static void dropsnap(struct client *c, int x, int y)
{
	int top = monat(x, y - EDGE - 1) < 0;
	int bottom = monat(x, y + EDGE + 1) < 0;
	int left = monat(x - EDGE - 1, y) < 0;
	int right = monat(x + EDGE + 1, y) < 0;
	int nt = monat(x, y - CORNER) < 0, nb = monat(x, y + CORNER) < 0;
	int nl = monat(x - CORNER, y) < 0, nr = monat(x + CORNER, y) < 0;
	int st = FLOAT;

	if (c->fixed)
		return;
	if ((top && nl) || (left && nt))
		st = TL;
	else if ((top && nr) || (right && nt))
		st = TR;
	else if ((bottom && nl) || (left && nb))
		st = BL;
	else if ((bottom && nr) || (right && nb))
		st = BR;
	else if (top)
		st = MAX;
	else if (left)
		st = LEFT;
	else if (right)
		st = RIGHT;
	if (st == FLOAT)
		return;
	c->state = st;
	arrange(c, nearmon(x, y));
	if (st == LEFT || st == RIGHT)
		assiststart(c);
}

static void movemouse(struct client *c, int px, int py)
{
	XEvent ev;
	Time last = 0;
	int ox, oy, x = px, y = py;

	if (c->fullscreen || !grabdrag(cur_move))
		return;

	/*
	 * A snapped window comes off its zone at its floating size, centred
	 * under the pointer and with the pointer still the same distance
	 * below its top edge, so a client-side title bar stays in hand.
	 */
	if (c->state != FLOAT) {
		struct rect f = floatrect(c);
		int gy = min(max(py - c->y, 0), f.h - 1);

		c->state = FLOAT;
		resize(c, px - f.w / 2, py - gy, f.w, f.h);
		setnetstate(c);
	}
	ox = c->x - px;
	oy = c->y - py;
	do {
		nextdrag(&ev);
		x = ev.type == MotionNotify ? ev.xmotion.x_root :
					      ev.xbutton.x_root;
		y = ev.type == MotionNotify ? ev.xmotion.y_root :
					      ev.xbutton.y_root;
		if (ev.type == MotionNotify && ev.xmotion.time - last < 8)
			continue;
		last = ev.xmotion.time;
		resize(c, x + ox, y + oy, c->w, c->h);
	} while (ev.type != ButtonRelease);
	XUngrabPointer(dpy, CurrentTime);
	dropsnap(c, x, y);
}

static void resizemouse(struct client *c, int dir, int px, int py)
{
	int l = dir == SZ_TL || dir == SZ_BL || dir == SZ_L;
	int r = dir == SZ_TR || dir == SZ_R || dir == SZ_BR;
	int t = dir == SZ_TL || dir == SZ_T || dir == SZ_TR;
	int b = dir == SZ_BR || dir == SZ_B || dir == SZ_BL;
	struct rect g = { c->x, c->y, c->w, c->h }, n;
	XEvent ev;
	Time last = 0;
	int dx, dy;

	if (c->fullscreen || c->fixed || !grabdrag(cur_resize))
		return;
	/* resizing by hand leaves it floating, wherever it ends up */
	c->state = FLOAT;
	setnetstate(c);
	do {
		nextdrag(&ev);
		if (ev.type != MotionNotify ||
		    ev.xmotion.time - last < 8)
			continue;
		last = ev.xmotion.time;
		dx = ev.xmotion.x_root - px;
		dy = ev.xmotion.y_root - py;
		n = g;
		if (l) {
			n.x = min(g.x + dx, g.x + g.w - MINSIZE);
			n.w = g.x + g.w - n.x;
		}
		if (r)
			n.w = max(g.w + dx, MINSIZE);
		if (t) {
			n.y = min(g.y + dy, g.y + g.h - MINSIZE);
			n.h = g.y + g.h - n.y;
		}
		if (b)
			n.h = max(g.h + dy, MINSIZE);
		resize(c, n.x, n.y, n.w, n.h);
	} while (ev.type != ButtonRelease);
	XUngrabPointer(dpy, CurrentTime);
}

/*
 * Resize grips: input-only strips just outside the focused window, there
 * being no frame to grab. Corners sit on top and reach a little inside.
 */
static void placegrips(void)
{
	struct client *c = sel;
	struct rect r[8];
	XWindowChanges wc;
	int i, g = GRIP, k = GRIPCORNER, x1, y1;

	if (!grips[0])
		return;
	if (!c || c->minimized || c->fullscreen || c->fixed || c->state == MAX) {
		for (i = 0; i < 8; i++)
			XUnmapWindow(dpy, grips[i]);
		return;
	}
	x1 = c->x + c->w;
	y1 = c->y + c->h;
	r[SZ_TL] = (struct rect){ c->x - g, c->y - g, k, k };
	r[SZ_T] = (struct rect){ c->x, c->y - g, c->w, g };
	r[SZ_TR] = (struct rect){ x1 + g - k, c->y - g, k, k };
	r[SZ_R] = (struct rect){ x1, c->y, g, c->h };
	r[SZ_BR] = (struct rect){ x1 + g - k, y1 + g - k, k, k };
	r[SZ_B] = (struct rect){ c->x, y1, c->w, g };
	r[SZ_BL] = (struct rect){ c->x - g, y1 + g - k, k, k };
	r[SZ_L] = (struct rect){ c->x - g, c->y, g, c->h };

	/* each lands just above the window, so the last one stacked is lowest */
	wc.sibling = c->win;
	wc.stack_mode = Above;
	for (i = 0; i < 8; i++) {
		int d = i < 4 ? 2 * i : 2 * (i - 4) + 1;	/* corners first */

		XMoveResizeWindow(dpy, grips[d], r[d].x, r[d].y, r[d].w, r[d].h);
		XConfigureWindow(dpy, grips[d], CWSibling | CWStackMode, &wc);
		XMapWindow(dpy, grips[d]);
	}
}

/* Super+right drag resizes from whichever edge or corner is nearest. */
static int nearedge(struct client *c, int x, int y)
{
	static const int dirs[3][3] = {
		{ SZ_TL, SZ_T,  SZ_TR },
		{ SZ_L,  SZ_BR, SZ_R },
		{ SZ_BL, SZ_B,  SZ_BR },
	};
	int col = min(max((x - c->x) * 3 / max(c->w, 1), 0), 2);
	int row = min(max((y - c->y) * 3 / max(c->h, 1), 0), 2);

	return dirs[row][col];
}

/* Alt+Tab */

static void gettitle(Window w, char *buf, size_t size)
{
	XTextProperty tp;
	char **list;
	int n;

	buf[0] = '\0';
	if (!XGetTextProperty(dpy, w, &tp, atom[NET_WM_NAME]) || !tp.nitems)
		if (!XGetTextProperty(dpy, w, &tp, XA_WM_NAME) || !tp.nitems)
			return;
	/*
	 * STRING is Latin-1 by the book, but xsetroot -name and friends put
	 * UTF-8 in it, so take it as is when it is valid UTF-8.
	 */
	if (tp.encoding == atom[UTF8_STRING] ||
	    (tp.encoding == XA_STRING &&
	     FcUtf8Len(tp.value, tp.nitems, &n, &n))) {
		snprintf(buf, size, "%.*s", (int)tp.nitems, (char *)tp.value);
	} else if (XmbTextPropertyToTextList(dpy, &tp, &list, &n) >= Success &&
		   n > 0 && *list) {
		snprintf(buf, size, "%s", *list);
		XFreeStringList(list);
	}
	XFree(tp.value);
}

/*
 * The font to draw codepoint u in: ours, or one fontconfig finds that has
 * it (emoji in a title, say). Colour fonts are skipped, Xft cannot draw
 * them.
 */
static XftFont *fontfor(FcChar32 u)
{
	FcPattern *pat, *match = NULL;
	FcCharSet *cs, *has;
	FcFontSet *fs;
	FcResult res;
	FcBool color;
	XftFont *f;
	int i;

	if (XftCharExists(dpy, font, u))
		return font;
	for (i = 0; i < nfallback; i++)
		if (XftCharExists(dpy, fallback[i], u))
			return fallback[i];
	for (i = 0; i < nnofont; i++)
		if (nofont[i] == u)
			return font;
	if (nfallback == MAXFALLBACK)
		return font;

	cs = FcCharSetCreate();
	FcCharSetAddChar(cs, u);
	pat = FcPatternDuplicate(font->pattern);
	FcPatternDel(pat, FC_FAMILY);
	FcPatternAddCharSet(pat, FC_CHARSET, cs);
	FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	/* the best match alone may well be the colour emoji font */
	if ((fs = FcFontSort(NULL, pat, FcTrue, NULL, &res))) {
		for (i = 0; i < fs->nfont && !match; i++) {
			color = FcFalse;
			FcPatternGetBool(fs->fonts[i], FC_COLOR, 0, &color);
			if (!color && FcPatternGetCharSet(fs->fonts[i], FC_CHARSET,
						0, &has) == FcResultMatch &&
			    FcCharSetHasChar(has, u))
				match = FcFontRenderPrepare(NULL, pat,
							    fs->fonts[i]);
		}
		FcFontSetDestroy(fs);
	}
	FcCharSetDestroy(cs);
	FcPatternDestroy(pat);

	f = match ? XftFontOpenPattern(dpy, match) : NULL;
	if (f && XftCharExists(dpy, f, u)) {
		fallback[nfallback++] = f;
		return f;
	}
	if (f)
		XftFontClose(dpy, f);
	else if (match)
		FcPatternDestroy(match);
	if (nnofont < (int)LEN(nofont))
		nofont[nnofont++] = u;
	return font;
}

/*
 * Measure n bytes of s, drawing them too when d is set, a run of
 * characters at a time that the same font has. Returns the width.
 */
static int inkrun(XftDraw *d, XftColor *col, int x, int y, const char *s,
		  size_t n, int *inkl, int *inkr)
{
	XftFont *f, *rf;
	XGlyphInfo g;
	FcChar32 u;
	size_t i = 0, start;
	int len, w = 0;

	if (inkl)
		*inkl = INT_MAX;
	if (inkr)
		*inkr = 0;

	while (i < n) {
		start = i;
		rf = NULL;
		while (i < n) {
			len = FcUtf8ToUcs4((FcChar8 *)s + i, &u, n - i);
			if (len <= 0) {
				len = 1;
				u = '?';
			}
			f = fontfor(u);
			if (rf && f != rf)
				break;
			rf = f;
			i += len;
		}
		XftTextExtentsUtf8(dpy, rf, (FcChar8 *)s + start, i - start, &g);
		if (d)
			XftDrawStringUtf8(d, col, rf, x + w, y,
					  (FcChar8 *)s + start, i - start);
		/* the ink box: g.x is how far the origin sits into it */
		if (inkl && g.width)
			*inkl = min(*inkl, w - g.x);
		if (inkr && g.width)
			*inkr = max(*inkr, w - g.x + g.width);
		w += g.xOff;
	}
	if (inkl && *inkl == INT_MAX)
		*inkl = 0;
	return w;
}

static int textrun(XftDraw *d, XftColor *col, int x, int y, const char *s,
		   size_t n)
{
	return inkrun(d, col, x, y, s, n, NULL, NULL);
}

/*
 * Draw s so its ink, not its advance, ends at right, and return where the
 * ink starts. Spacing by ink keeps gaps even whatever the glyphs carry
 * as side bearing: a 7 at the end of the clock, the power symbol.
 */
static int drawright(XftDraw *d, int right, int y, const char *s)
{
	int l, r;

	inkrun(NULL, NULL, 0, 0, s, strlen(s), &l, &r);
	inkrun(d, &xc_fg, right - r, y, s, strlen(s), NULL, NULL);
	return right - r + l;
}

static void drawtext(XftDraw *d, XftColor *col, int x, int y, const char *s)
{
	textrun(d, col, x, y, s, strlen(s));
}

/* Cut s, at a character boundary, until it is at most maxw px wide. */
static void fittext(char *s, int maxw)
{
	size_t n = strlen(s);

	for (;;) {
		if (textrun(NULL, NULL, 0, 0, s, n) <= maxw || !n)
			break;
		do
			n--;
		while (n && (s[n] & 0xc0) == 0x80);
	}
	s[n] = '\0';
}

/*
 * Scale what the window shows now into its thumbnail. Returns whether it
 * has one, fresh or kept from before it was minimized.
 */
static int updatethumb(struct client *c)
{
	XRenderPictureAttributes pa = { .subwindow_mode = IncludeInferiors };
	XWindowAttributes wa;
	XRenderPictFormat *fmt;
	XTransform xf = { { { 0 } } };
	Picture src;
	Pixmap pix;
	double sc;
	int tw, th;

	if (!composite)
		return 0;
	if (!c->thumb) {
		c->thumb = XCreatePixmap(dpy, root, THUMBW, THUMBH,
					 DefaultDepth(dpy, scr));
		c->thumbpic = XRenderCreatePicture(dpy, c->thumb, rootfmt, 0,
						   NULL);
	}
	if (!c->redirected || c->minimized ||
	    !XGetWindowAttributes(dpy, c->win, &wa) ||
	    wa.map_state != IsViewable ||
	    !(fmt = XRenderFindVisualFormat(dpy, wa.visual)))
		return c->hasthumb;

	pix = XCompositeNameWindowPixmap(dpy, c->win);
	src = XRenderCreatePicture(dpy, pix, fmt, CPSubwindowMode, &pa);
	sc = max(1.0, max((double)wa.width / THUMBW,
			  (double)wa.height / THUMBH));
	tw = wa.width / sc;
	th = wa.height / sc;
	xf.matrix[0][0] = XDoubleToFixed(sc);
	xf.matrix[1][1] = XDoubleToFixed(sc);
	xf.matrix[2][2] = XDoubleToFixed(1);
	XRenderSetPictureTransform(dpy, src, &xf);
	XRenderSetPictureFilter(dpy, src, FilterBilinear, NULL, 0);
	XRenderFillRectangle(dpy, PictOpSrc, c->thumbpic, &xc_box.color, 0, 0,
			     THUMBW, THUMBH);
	XRenderComposite(dpy, PictOpSrc, src, None, c->thumbpic, 0, 0, 0, 0,
			 (THUMBW - tw) / 2, (THUMBH - th) / 2, tw, th);
	XRenderFreePicture(dpy, src);
	XFreePixmap(dpy, pix);
	c->hasthumb = 1;
	return 1;
}

/* A card per window: thumbnail, title under it, in rows that fit. */
static void drawtab(void)
{
	struct rect r = tabassist ? zonerect(assistzone, assistmon) :
			mons[pointermon()];
	int pad = 10, lh = font->ascent + font->descent + 8;
	int cw = THUMBW + 2 * pad, ch = THUMBH + lh + 2 * pad;
	int cols = max(min(ntab, (r.w - 4 * pad) / cw), 1);
	int rows = (ntab + cols - 1) / cols;
	int w = cols * cw + 2 * pad, h = rows * ch + 2 * pad, i, x, y;
	int ox = pad, oy = pad;
	char title[512];

	/* snap assist covers its whole half, the cards centred on it */
	if (tabassist) {
		ox = (r.w - w) / 2 + pad;
		oy = (r.h - h) / 2 + pad;
		w = r.w;
		h = r.h;
	}
	tabx = r.x + (r.w - w) / 2;
	taby = r.y + (r.h - h) / 2;
	XMoveResizeWindow(dpy, tabwin, tabx, taby, w, h);
	XMapRaised(dpy, tabwin);
	if (tabassist && assistbgpic)
		XRenderComposite(dpy, PictOpSrc, assistbgpic, None, tabpic,
				 0, 0, 0, 0, 0, 0, w, h);
	else
		XftDrawRect(tabdraw, &xc_bg, 0, 0, w, h);
	for (i = 0; i < ntab; i++) {
		x = ox + i % cols * cw;
		y = oy + i / cols * ch;
		if (tabassist && i != tabsel)
			XftDrawRect(tabdraw, &xc_bg, x, y, cw, ch);
		cards[i] = (struct rect){ tabx + x, taby + y, cw, ch };
		if (i == tabsel)
			XftDrawRect(tabdraw, &xc_sel, x, y, cw, ch);
		if (updatethumb(tab[i]))
			XRenderComposite(dpy, PictOpSrc, tab[i]->thumbpic, None,
					 tabpic, 0, 0, 0, 0, x + pad, y + pad,
					 THUMBW, THUMBH);
		else
			XftDrawRect(tabdraw, &xc_box, x + pad, y + pad,
				    THUMBW, THUMBH);
		gettitle(tab[i]->win, title, sizeof(title));
		if (!*title)
			strcpy(title, "(untitled)");
		fittext(title, THUMBW);
		drawtext(tabdraw, tab[i]->minimized && i != tabsel ?
			 &xc_dim : &xc_fg, x + pad,
			 y + pad + THUMBH + (lh + font->ascent - font->descent) / 2,
			 title);
	}
}

/*
 * The list shows at once, no delay (alttab-instant@nybo). It holds the
 * keyboard until Alt goes up, so Tab, Q and Esc reach it.
 */
static void tabstart(int step)
{
	struct client *c;
	Window dw;
	int di;
	unsigned int mask;

	ntab = 0;
	for (c = clients; c && ntab < MAXTAB; c = c->next)
		tab[ntab++] = c;
	if (!ntab || launching)
		return;
	if (XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync,
			  CurrentTime) != GrabSuccess)
		return;
	tabbing = 1;
	/* with nothing focused the first entry is not "here" to skip */
	tabsel = sel || step < 0 ? 0 : -1;
	tabsel = (tabsel + step + ntab) % ntab;
	drawtab();

	/* Alt was already let go before the grab took: a quick tap */
	XQueryPointer(dpy, root, &dw, &dw, &di, &di, &di, &di, &mask);
	if (!(mask & ALT))
		endtab(tab[tabsel]);
}

static void endtab(struct client *c)
{
	int assist = tabassist;

	tabbing = tabassist = 0;
	XUngrabKeyboard(dpy, CurrentTime);
	if (assist)
		XUngrabPointer(dpy, CurrentTime);
	if (assistbg) {
		XRenderFreePicture(dpy, assistbgpic);
		XFreePixmap(dpy, assistbg);
		assistbg = assistbgpic = 0;
	}
	XUnmapWindow(dpy, tabwin);
	if (!c)
		return;
	focus(c);
	if (assist && !c->fixed) {
		c->state = assistzone;
		arrange(c, assistmon);
	}
}

/*
 * Copy what the screen shows in r, darkened, for snap assist to sit on.
 * There is no compositor to blur with, but a dimmed half reads clearly
 * as "pick one for here".
 */
static void dimhalf(struct rect r)
{
	XRenderPictureAttributes pa = { .subwindow_mode = IncludeInferiors };
	XRenderColor shade = { 0, 0, 0, 0xa000 };
	Picture src;

	if (!composite)
		return;
	assistbg = XCreatePixmap(dpy, root, r.w, r.h, DefaultDepth(dpy, scr));
	assistbgpic = XRenderCreatePicture(dpy, assistbg, rootfmt, 0, NULL);
	src = XRenderCreatePicture(dpy, root, rootfmt, CPSubwindowMode, &pa);
	XRenderComposite(dpy, PictOpSrc, src, None, assistbgpic, r.x, r.y,
			 0, 0, 0, 0, r.w, r.h);
	XRenderFreePicture(dpy, src);
	XRenderFillRectangle(dpy, PictOpOver, assistbgpic, &shade, 0, 0,
			     r.w, r.h);
}

/*
 * Snap assist, as in Windows: c just took one half, and if nothing has the
 * other, the other windows show there as cards. The one picked, by
 * click or arrows and Enter, takes that half.
 */
static void assiststart(struct client *c)
{
	struct client *t;
	int m = clientmon(c), other = c->state == LEFT ? RIGHT : LEFT;

	if (tabbing || launching)
		return;
	for (t = clients; t; t = t->next)
		if (t != c && !t->minimized && t->state == other &&
		    clientmon(t) == m)
			return;
	ntab = 0;
	for (t = clients; t && ntab < MAXTAB; t = t->next)
		if (t != c && !t->fixed && !t->fullscreen && !find(t->transfor))
			tab[ntab++] = t;
	if (!ntab)
		return;
	if (XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync,
			  CurrentTime) != GrabSuccess)
		return;
	/* every click comes here: on a card it picks, elsewhere it cancels */
	if (XGrabPointer(dpy, root, False, ButtonPressMask, GrabModeAsync,
			 GrabModeAsync, None, cur_normal,
			 CurrentTime) != GrabSuccess) {
		XUngrabKeyboard(dpy, CurrentTime);
		return;
	}
	tabbing = tabassist = 1;
	assistzone = other;
	assistmon = m;
	tabsel = 0;
	dimhalf(zonerect(other, m));
	drawtab();
}

/*
 * Keys while snap assist shows. Returns 0 for a key it does not take,
 * which also puts it away, so Super+arrow and the rest carry on as usual.
 */
static int assistkey(KeySym sym, unsigned int state)
{
	if (IsModifierKey(sym))
		return 1;
	if (state & SUPER) {
		endtab(NULL);
		return 0;
	}
	switch (sym) {
	case XK_Right:
	case XK_Down:
	case XK_Tab:
		tabsel = (tabsel + (state & ShiftMask ? -1 : 1) + ntab) % ntab;
		drawtab();
		return 1;
	case XK_Left:
	case XK_Up:
		tabsel = (tabsel - 1 + ntab) % ntab;
		drawtab();
		return 1;
	case XK_Return:
	case XK_KP_Enter:
	case XK_space:
		endtab(tab[tabsel]);
		return 1;
	case XK_Escape:
		endtab(NULL);
		return 1;
	}
	endtab(NULL);
	return 0;
}

static void assistclick(int x, int y)
{
	int i;

	for (i = 0; i < ntab; i++)
		if (x >= cards[i].x && x < cards[i].x + cards[i].w &&
		    y >= cards[i].y && y < cards[i].y + cards[i].h) {
			endtab(tab[i]);
			return;
		}
	endtab(NULL);
}

static void tabkey(KeySym sym, unsigned int state)
{
	if (sym == XK_Tab) {
		tabsel = (tabsel + (state & ShiftMask ? -1 : 1) + ntab) % ntab;
		drawtab();
	} else if (sym == XK_Right || sym == XK_Left) {
		tabsel = (tabsel + (sym == XK_Right ? 1 : -1) + ntab) % ntab;
		drawtab();
	} else if (sym == XK_q) {
		/* the list drops it once it is really gone (alttab-close@nybo) */
		closewin(tab[tabsel]);
	} else if (sym == XK_Escape) {
		endtab(NULL);
	}
}

/* Bar */

static struct bar *findbar(Window w)
{
	int i;

	for (i = 0; i < nbars; i++)
		if (bars[i].win == w)
			return &bars[i];
	return NULL;
}

static long long nowms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/*
 * Hidden bars sit above the root window, off every monitor, rather than
 * being unmapped: an unmap would look like a dock going away. While a bar
 * is up its trigger is moved off as well, so the bottom pixel row still
 * clicks the bar.
 */
static void showbar(struct bar *b, int on)
{
	struct rect m = mons[b - bars];

	b->shown = on;
	XMoveWindow(dpy, b->win, m.x, on ? m.y + m.h - BARH : -2 * BARH);
	if (b->trigger)
		XMoveWindow(dpy, b->trigger, m.x, on ? -2 * BARH :
			    m.y + m.h - 1);
	XRaiseWindow(dpy, on ? b->win : b->trigger);
}

/* The hide delay ran out: put away the bars the pointer is not on. */
static void autohide(void)
{
	Window dw;
	int x, y, di, i;
	unsigned int mask;
	struct rect m;

	XQueryPointer(dpy, root, &dw, &dw, &x, &y, &di, &di, &mask);
	for (i = 0; i < nbars; i++) {
		m = mons[i];
		if (bars[i].shown && !(x >= m.x && x < m.x + m.w &&
				       y >= m.y + m.h - BARH && y < m.y + m.h))
			showbar(&bars[i], 0);
	}
}

static void adddock(Window w, long bottom, long x1, long x2)
{
	struct dock *d = finddock(w);

	if (!d && ndocks < MAXDOCKS)
		d = &docks[ndocks++];
	if (!d)
		return;
	memset(d, 0, sizeof(*d));
	d->win = w;
	d->strut[3] = bottom;
	d->strut[10] = x1;
	d->strut[11] = x2;
}

/*
 * A bar along the bottom of every monitor. Kept up, it reserves its space
 * like any dock would, so zones and maximize stop above it; hiding, it
 * reserves nothing. The first one also holds the tray, so it is never the
 * one to go when monitors do.
 */
static void placebars(void)
{
	XSetWindowAttributes wa = { 0 };
	struct bar *b;
	struct rect m;
	int i;

	if (!BARH)
		return;
	while (nbars > nmons) {
		b = &bars[--nbars];
		removedock(b->win);
		removedock(b->trigger);
		XftDrawDestroy(b->draw);
		XDestroyWindow(dpy, b->win);
		if (b->trigger)
			XDestroyWindow(dpy, b->trigger);
		memset(b, 0, sizeof(*b));
	}
	wa.override_redirect = True;
	for (i = 0; i < nmons; i++) {
		b = &bars[i];
		m = mons[i];
		if (!b->win) {
			wa.background_pixel = xc_bg.pixel;
			wa.event_mask = ExposureMask | ButtonPressMask |
					EnterWindowMask | LeaveWindowMask;
			b->win = XCreateWindow(dpy, root, 0, 0, 1, 1, 0,
					       CopyFromParent, InputOutput,
					       CopyFromParent,
					       CWOverrideRedirect |
					       CWBackPixel | CWEventMask, &wa);
			b->draw = XftDrawCreate(dpy, b->win,
						DefaultVisual(dpy, scr),
						DefaultColormap(dpy, scr));
			if (AUTOHIDE) {
				wa.event_mask = EnterWindowMask;
				b->trigger = XCreateWindow(dpy, root, 0, 0, 1,
							   1, 0, 0, InputOnly,
							   CopyFromParent,
							   CWOverrideRedirect |
							   CWEventMask, &wa);
				XMapWindow(dpy, b->trigger);
			}
			XMapWindow(dpy, b->win);
			nbars = i + 1;
		}
		XResizeWindow(dpy, b->win, m.w, BARH);
		if (b->trigger)
			XResizeWindow(dpy, b->trigger, m.w, 1);
		showbar(b, !AUTOHIDE || b->shown);
		/* in the dock list either way, to be kept on top */
		adddock(b->win, AUTOHIDE ? 0 : sh - (m.y + m.h - BARH),
			m.x, m.x + m.w - 1);
		if (b->trigger)
			adddock(b->trigger, 0, 0, 0);
	}
	bardirty = 1;
}

/*
 * "48%", or with CHARGING in front while on AC; empty without a battery.
 * Several batteries are averaged. Read fresh at every draw, which the
 * clock makes at least once a minute.
 */
static void battery(char *buf, size_t size)
{
	char path[PATH_MAX], v[32];
	struct dirent *e;
	int sum = 0, n = 0, ac = 0;
	FILE *f;
	DIR *d;

	buf[0] = '\0';
	if (!(d = opendir(PSU)))
		return;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), PSU "/%s/type", e->d_name);
		if (!(f = fopen(path, "r")))
			continue;
		v[0] = '\0';
		if (!fgets(v, sizeof(v), f))
			v[0] = '\0';
		fclose(f);
		if (!strncmp(v, "Battery", 7))
			snprintf(path, sizeof(path), PSU "/%s/capacity",
				 e->d_name);
		else if (!strncmp(v, "Mains", 5))
			snprintf(path, sizeof(path), PSU "/%s/online",
				 e->d_name);
		else
			continue;
		if (!(f = fopen(path, "r")))
			continue;
		if (fgets(v, sizeof(v), f)) {
			if (strstr(path, "/capacity")) {
				sum += atoi(v);
				n++;
			} else {
				ac |= atoi(v);
			}
		}
		fclose(f);
	}
	closedir(d);
	if (n)
		snprintf(buf, size, "%s%d%%", ac ? CHARGING : "", sum / n);
}

/*
 * "2.7G": memory in use, what the kernel cannot hand out at once,
 * "0.4G": disk swap in use and "0.2G": what zram holds, uncompressed;
 * each left empty when there is none of that kind.
 */
static void memory(char *mem, char *swap, char *zram, size_t n)
{
	char line[256], name[128], type[32];
	long total = 0, avail = 0, v, sz, used, disk = 0, z = 0;
	int hasdisk = 0, hasz = 0;
	FILE *f;

	mem[0] = swap[0] = zram[0] = '\0';
	if ((f = fopen("/proc/meminfo", "r"))) {
		while (fgets(line, sizeof(line), f)) {
			if (sscanf(line, "MemTotal: %ld", &v) == 1)
				total = v;
			else if (sscanf(line, "MemAvailable: %ld", &v) == 1)
				avail = v;
		}
		fclose(f);
	}
	if (total)
		snprintf(mem, n, "%.1fG", (total - avail) / 1048576.0);
	if (!(f = fopen("/proc/swaps", "r")))
		return;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "%127s %31s %ld %ld", name, type, &sz, &used) == 4) {
			if (!strncmp(name, "/dev/zram", 9))
				z += used, hasz = 1;
			else
				disk += used, hasdisk = 1;
		}
	fclose(f);
	if (hasdisk)
		snprintf(swap, n, "%.1fG", disk / 1048576.0);
	if (hasz)
		snprintf(zram, n, "%.1fG", z / 1048576.0);
}

static void drawonebar(int m)
{
	struct bar *b = &bars[m];
	struct client *c, *list[MAXTAB], *t;
	char status[512], line[512], clk[64], bat[32], mem[32], swap[32], zram[32];
	int pad = 8, n = 0, i, j, x, w, right;
	int base = (BARH + font->ascent - font->descent) / 2;
	int bw = mons[m].w;
	time_t now = time(NULL);

	XftDrawRect(b->draw, &xc_bg, 0, 0, bw, BARH);

	/*
	 * right to left, BARGAP of empty bar between the ink of each: power
	 * button, clock, battery, memory, swap, zram, then on the first bar sound, tray icons
	 * and status. Icons get a little more, since a thin glyph with the
	 * same gap looks closer than text does.
	 */
	right = drawright(b->draw, bw - BARGAP, base, POWER) - BARGAP;
	b->powerx = right + BARGAP / 2;
	strftime(clk, sizeof(clk), CLOCK, localtime(&now));
	right = drawright(b->draw, right, base, clk) - BARGAP;
	battery(bat, sizeof(bat));
	if (*bat)
		right = drawright(b->draw, right, base, bat) - BARGAP;
	memory(mem, swap, zram, sizeof(mem));
	if (m == 0 && *mem)
		right = drawright(b->draw, right, base, mem) - BARGAP;
	if (m == 0 && *swap)
		right = drawright(b->draw, right, base, swap) - BARGAP;
	if (m == 0 && *zram)
		right = drawright(b->draw, right, base, zram) - BARGAP;
	/* the sound button sits with the tray icons, which it stands in for */
	b->soundx = b->soundend = 0;
	if (m == 0) {
		b->soundend = right + BARGAP / 2;
		right = drawright(b->draw, right, base, SOUND) - BARGAP;
		b->soundx = right + BARGAP / 2;
	}
	for (i = ntray - 1; m == 0 && i >= 0; i--) {
		if (!tray[i].mapped)
			continue;
		/* icons fill their window but for about TRAYPAD on each side */
		right -= TRAYSZ - TRAYPAD + 2;
		if (tray[i].x != right) {
			tray[i].x = right;
			XMoveResizeWindow(dpy, tray[i].win, right,
					  (BARH - TRAYSZ) / 2, TRAYSZ, TRAYSZ);
		}
		right += TRAYPAD - BARGAP - 2;
	}
	if (m == 0) {
		gettitle(root, status, sizeof(status));
		if (*status) {
			fittext(status, bw / 3);
			right = drawright(b->draw, right, base, status) - BARGAP;
		}
	}

	/*
	 * left: a button per window on this monitor, in the order they
	 * opened, the way a taskbar keeps its place. Dialogs ride along with
	 * their parent.
	 */
	for (c = clients; c && n < MAXTAB; c = c->next) {
		if (find(c->transfor) || clientmon(c) != m)
			continue;
		for (j = n++; j > 0 && list[j - 1]->seq > c->seq; j--)
			list[j] = list[j - 1];
		list[j] = c;
	}
	w = n ? min(BARBTN, (right - 2 * pad) / n) : 0;
	b->nbtn = 0;
	for (i = 0, x = pad / 2; i < n && w > 2 * pad; i++, x += w) {
		t = list[i];
		if (t == sel)
			XftDrawRect(b->draw, &xc_sel, x, 3, w - 4, BARH - 6);
		gettitle(t->win, line, sizeof(line));
		if (!*line)
			strcpy(line, "(untitled)");
		fittext(line, w - 4 - 2 * pad);
		drawtext(b->draw, t->minimized ? &xc_dim : &xc_fg, x + pad,
			 base, line);
		b->btn[b->nbtn].x = x;
		b->btn[b->nbtn].w = w;
		b->btn[b->nbtn++].c = t;
	}
}

static void drawbar(void)
{
	int i;

	for (i = 0; i < nbars; i++)
		drawonebar(i);
}

/*
 * Click a window to bring it up, or the focused one to minimize it; the
 * power button opens the session entries, the sound one the sound settings.
 */
static void barclick(struct bar *b, int x)
{
	struct client *c;
	int i;

	if (x >= b->powerx) {
		launchstart(1);
		return;
	}
	if (x >= b->soundx && x < b->soundend) {
		spawn(SOUNDCMD);
		return;
	}
	for (i = 0; i < b->nbtn; i++) {
		if (x < b->btn[i].x || x >= b->btn[i].x + b->btn[i].w)
			continue;
		c = b->btn[i].c;
		if (c == sel && !c->minimized)
			minimize(c);
		else
			focus(c);
		return;
	}
}

/* System tray */

static int findtray(Window w)
{
	int i;

	for (i = 0; i < ntray; i++)
		if (tray[i].win == w)
			return i;
	return -1;
}

/* An icon says through _XEMBED_INFO whether it wants to be shown. */
static int traywantsmap(Window w)
{
	unsigned long n;
	long *v = getlongs(w, atom[XEMBED_INFO], atom[XEMBED_INFO], &n);
	int on = 1;

	if (v && n >= 2)
		on = v[1] & 1;	/* XEMBED_MAPPED */
	if (v)
		XFree(v);
	return on;
}

static void traydock(Window w)
{
	XWindowAttributes wa;
	XEvent ev = { 0 };
	struct client *c;

	if (ntray >= MAXTRAY || findtray(w) >= 0 ||
	    !XGetWindowAttributes(dpy, w, &wa))
		return;
	/*
	 * An icon may already be mapped when it asks, even managed as a
	 * window: GTK maps a fresh one in the gap while nwm restarts. Let go
	 * of it quietly, since unmanaging it the usual way would clear the
	 * events selected on it below.
	 */
	if ((c = find(w)))
		unmanage(c, 1);
	tray[ntray].win = w;
	tray[ntray].x = -1;
	tray[ntray].mapped = 0;
	ntray++;
	XSelectInput(dpy, w, StructureNotifyMask | PropertyChangeMask);
	/* back to the root window, not destroyed, if nwm goes away first */
	XAddToSaveSet(dpy, w);
	XReparentWindow(dpy, w, TRAYWIN, 0, 0);
	XResizeWindow(dpy, w, TRAYSZ, TRAYSZ);

	ev.type = ClientMessage;
	ev.xclient.window = w;
	ev.xclient.message_type = atom[XEMBED];
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = CurrentTime;
	ev.xclient.data.l[1] = 0;	/* XEMBED_EMBEDDED_NOTIFY */
	ev.xclient.data.l[3] = TRAYWIN;
	XSendEvent(dpy, w, False, NoEventMask, &ev);
	/* no MapNotify comes for one that was mapped already, so track it here */
	tray[ntray - 1].mapped = traywantsmap(w);
	if (tray[ntray - 1].mapped)
		XMapRaised(dpy, w);
	else
		XUnmapWindow(dpy, w);
	bardirty = 1;
}

static int trayremove(Window w)
{
	int i = findtray(w);

	if (i < 0)
		return 0;
	memmove(&tray[i], &tray[i + 1], (ntray - i - 1) * sizeof(*tray));
	ntray--;
	bardirty = 1;
	return 1;
}

static void traymapped(Window w, int on)
{
	int i = findtray(w);

	if (i < 0)
		return;
	tray[i].mapped = on;
	tray[i].x = -1;
	bardirty = 1;
}

/* Take the tray selection and tell running apps there is a tray now. */
static void traystart(void)
{
	long traycolors[12] = {
		xc_fg.color.red, xc_fg.color.green, xc_fg.color.blue,
		0xffff, 0x5555, 0x5555,
		0xf0f0, 0xc6c6, 0x7474,
		0x8a8a, 0xe2e2, 0x3434,
	};
	XEvent ev = { 0 };
	char name[32];
	long v = 0;

	snprintf(name, sizeof(name), "_NET_SYSTEM_TRAY_S%d", scr);
	traysel = XInternAtom(dpy, name, False);
	XSetSelectionOwner(dpy, traysel, TRAYWIN, CurrentTime);
	if (XGetSelectionOwner(dpy, traysel) != TRAYWIN) {
		fputs("nwm: could not take the tray selection\n", stderr);
		return;
	}
	XChangeProperty(dpy, TRAYWIN, atom[NET_SYSTEM_TRAY_ORIENTATION],
			XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)&v, 1);
	/*
	 * Symbolic icons are drawn in these: foreground, error, warning,
	 * success. Without them GTK uses the theme's text colour, dark on a
	 * light theme, which is black on this bar.
	 */
	XChangeProperty(dpy, TRAYWIN, atom[NET_SYSTEM_TRAY_COLORS],
			XA_CARDINAL, 32, PropModeReplace,
			(unsigned char *)traycolors, 12);
	ev.type = ClientMessage;
	ev.xclient.window = root;
	ev.xclient.message_type = atom[MANAGER];
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = CurrentTime;
	ev.xclient.data.l[1] = traysel;
	ev.xclient.data.l[2] = TRAYWIN;
	XSendEvent(dpy, root, False, StructureNotifyMask, &ev);
}

/* Launcher */

/* Is any of our desktop names in a "A;B;" list from a desktop file? */
static int desktopin(const char *list)
{
	char names[256], *name, *save;
	const char *p;
	size_t n;

	snprintf(names, sizeof(names), "%s", getenv("XDG_CURRENT_DESKTOP"));
	for (name = strtok_r(names, ":", &save); name;
	     name = strtok_r(NULL, ":", &save)) {
		n = strlen(name);
		for (p = list; (p = strstr(p, name)); p += n)
			if ((p == list || p[-1] == ';') &&
			    (p[n] == ';' || p[n] == '\0'))
				return 1;
	}
	return 0;
}

/* TryExec: is the program there, as a path or somewhere on PATH? */
static int haveprog(const char *prog)
{
	char path[PATH_MAX], dirs[4096], *dir, *save;

	if (strchr(prog, '/'))
		return !access(prog, X_OK);
	snprintf(dirs, sizeof(dirs), "%s", getenv("PATH") ? getenv("PATH") : "");
	for (dir = strtok_r(dirs, ":", &save); dir;
	     dir = strtok_r(NULL, ":", &save)) {
		snprintf(path, sizeof(path), "%s/%s", dir, prog);
		if (!access(path, X_OK))
			return 1;
	}
	return 0;
}

static void loadapp(const char *path, const char *id)
{
	struct app a = { 0 };
	char line[1024], *v, *s, *d;
	int i, group = 0, term = 0;
	size_t n;
	FILE *f;

	for (i = 0; i < napps; i++)
		if (!strcmp(apps[i].id, id))
			return;
	if (napps >= MAXAPPS || !(f = fopen(path, "r")))
		return;
	snprintf(a.id, sizeof(a.id), "%s", id);
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			group = !strcmp(line, "[Desktop Entry]");
			continue;
		}
		if (!group || !(v = strchr(line, '=')))
			continue;
		*v++ = '\0';
		if (!strcmp(line, "Name"))
			snprintf(a.name, sizeof(a.name), "%s", v);
		else if (!strcmp(line, "Exec"))
			snprintf(a.exec, sizeof(a.exec), "%s", v);
		else if (!strcmp(line, "Terminal"))
			term = !strcmp(v, "true");
		else if (!strcmp(line, "Hidden") && !strcmp(v, "true"))
			a.hidden = 1;
		else if (!strcmp(line, "NoDisplay") && !strcmp(v, "true"))
			a.nodisplay = 1;
		else if (!strcmp(line, "Type") && strcmp(v, "Application"))
			a.hidden = 1;
		else if (!strcmp(line, "OnlyShowIn") && !desktopin(v))
			a.elsewhere = 1;
		else if (!strcmp(line, "NotShowIn") && desktopin(v))
			a.elsewhere = 1;
		else if (!strcmp(line, "X-GNOME-Autostart-enabled") &&
			 !strcmp(v, "false"))
			a.elsewhere = 1;
		else if (!strcmp(line, "TryExec") && !haveprog(v))
			a.elsewhere = 1;
	}
	fclose(f);
	if (!*a.name || !*a.exec)
		a.hidden = 1;

	/* drop the %f %U ... field codes, keep %% as % */
	for (s = d = a.exec; *s; s++) {
		if (*s != '%')
			*d++ = *s;
		else if (s[1] && s[1] != '%')
			s++;
		else if (s[1])
			*d++ = *++s;
	}
	*d = '\0';
	n = strlen(TERMINAL " ");
	if (term && strlen(a.exec) + n < sizeof(a.exec)) {
		memmove(a.exec + n, a.exec, strlen(a.exec) + 1);
		memcpy(a.exec, TERMINAL " ", n);
	}
	apps[napps++] = a;
}

static void loaddir(const char *dir)
{
	char path[PATH_MAX];
	struct dirent *e;
	size_t n;
	DIR *d;

	if (!(d = opendir(dir)))
		return;
	while ((e = readdir(d))) {
		n = strlen(e->d_name);
		if (n < 8 || strcmp(e->d_name + n - 8, ".desktop"))
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		loadapp(path, e->d_name);
	}
	closedir(d);
}

static int appcmp(const void *a, const void *b)
{
	return strcasecmp(((const struct app *)a)->name,
			  ((const struct app *)b)->name);
}

/* Read the apps fresh each time, so new installs show up at once. */
static void loadapps(void)
{
	static const char *session[][2] = {
		{ "Lock screen", LOCK },
		{ "Log out", NULL },
		{ "Suspend", "systemctl suspend" },
		{ "Restart", "systemctl reboot" },
		{ "Shut down", "systemctl poweroff" },
	};
	const char *home = getenv("HOME"), *data = getenv("XDG_DATA_HOME");
	char dir[PATH_MAX];
	struct app *a;
	size_t i;

	napps = 0;
	if (!apps && !(apps = malloc(MAXAPPS * sizeof(*apps))))
		return;
	/* the user's own entries come first and override the system's */
	if (data && *data)
		snprintf(dir, sizeof(dir), "%s/applications", data);
	else
		snprintf(dir, sizeof(dir), "%s/.local/share/applications",
			 home ? home : "");
	loaddir(dir);
	loaddir("/usr/local/share/applications");
	loaddir("/usr/share/applications");
	loaddir("/var/lib/flatpak/exports/share/applications");

	/* the session, as entries of its own: log out is quitting nwm */
	for (i = 0; i < LEN(session) && napps < MAXAPPS; i++) {
		a = &apps[napps++];
		memset(a, 0, sizeof(*a));
		a->session = 1;
		snprintf(a->name, sizeof(a->name), "%s", session[i][0]);
		if (session[i][1])
			snprintf(a->exec, sizeof(a->exec), "%s", session[i][1]);
		else
			snprintf(a->exec, sizeof(a->exec), "kill -TERM %d",
				 (int)getpid());
	}
	qsort(apps, napps, sizeof(*apps), appcmp);
}

/*
 * XDG autostart, the user's entries first so they can switch the system's
 * off. Entries meant only for other desktops are left alone.
 */
static void autostart(void)
{
	const char *home = getenv("HOME"), *cfg = getenv("XDG_CONFIG_HOME");
	const char *sys = getenv("XDG_CONFIG_DIRS");
	char dir[PATH_MAX], dirs[4096], *d, *save;
	int i;

	napps = 0;
	if (!apps && !(apps = malloc(MAXAPPS * sizeof(*apps))))
		return;
	if (cfg && *cfg)
		snprintf(dir, sizeof(dir), "%s/autostart", cfg);
	else
		snprintf(dir, sizeof(dir), "%s/.config/autostart",
			 home ? home : "");
	loaddir(dir);
	snprintf(dirs, sizeof(dirs), "%s", sys && *sys ? sys : "/etc/xdg");
	for (d = strtok_r(dirs, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
		snprintf(dir, sizeof(dir), "%s/autostart", d);
		loaddir(dir);
	}
	for (i = 0; i < napps; i++)
		if (!apps[i].hidden && !apps[i].elsewhere)
			spawn(apps[i].exec);
	napps = 0;
}

/* Names starting with the query first, then names containing it. */
static void filter(void)
{
	int pass, i;
	char *p;

	nmatch = 0;
	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < napps && nmatch < LAUNCHROWS; i++) {
			if (apps[i].hidden || apps[i].nodisplay ||
			    apps[i].elsewhere ||
			    (launchsession && !apps[i].session) ||
			    !(p = strcasestr(apps[i].name, query)))
				continue;
			if ((p == apps[i].name) == !pass)
				match[nmatch++] = i;
		}
	launchsel = 0;
}

static void drawlaunch(void)
{
	struct rect r = mons[pointermon()];
	int pad = 8, lh = font->ascent + font->descent + 12;
	int w = min(max(r.w * 2 / 5, 360), r.w), h = (LAUNCHROWS + 1) * lh + 2 * pad;
	int i, y, base = (lh + font->ascent - font->descent) / 2;
	char line[sizeof(query) + 16];

	XMoveResizeWindow(dpy, tabwin, r.x + (r.w - w) / 2, r.y + r.h / 5, w, h);
	XMapRaised(dpy, tabwin);
	XftDrawRect(tabdraw, &xc_bg, 0, 0, w, h);
	snprintf(line, sizeof(line), "> %s_", query);
	fittext(line, w - 4 * pad);
	drawtext(tabdraw, &xc_fg, 2 * pad, pad + base, line);
	if (!nmatch && qlen) {
		y = pad + lh;
		XftDrawRect(tabdraw, &xc_sel, pad / 2, y, w - pad, lh);
		snprintf(line, sizeof(line), "run: %s", query);
		fittext(line, w - 4 * pad);
		drawtext(tabdraw, &xc_fg, 2 * pad, y + base, line);
	}
	for (i = 0; i < nmatch; i++) {
		y = pad + (i + 1) * lh;
		if (i == launchsel)
			XftDrawRect(tabdraw, &xc_sel, pad / 2, y, w - pad, lh);
		snprintf(line, sizeof(line), "%s", apps[match[i]].name);
		fittext(line, w - 4 * pad);
		drawtext(tabdraw, &xc_fg, 2 * pad, y + base, line);
	}
}

/* The launcher, or with session set just lock, log out and the rest. */
static void launchstart(int session)
{
	if (tabbing || launching)
		return;
	if (XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync,
			  CurrentTime) != GrabSuccess)
		return;
	launching = 1;
	launchsession = session;
	loadapps();
	query[0] = '\0';
	qlen = 0;
	filter();
	drawlaunch();
}

static void endlaunch(void)
{
	launching = 0;
	XUngrabKeyboard(dpy, CurrentTime);
	XUnmapWindow(dpy, tabwin);
}

static void launchkey(XKeyEvent *ev)
{
	char buf[32];
	KeySym sym;
	int n = XLookupString(ev, buf, sizeof(buf), &sym, NULL);

	switch (sym) {
	case XK_Escape:
	case XK_Super_L:
	case XK_Super_R:
		endlaunch();
		return;
	case XK_Return:
	case XK_KP_Enter:
		if (nmatch)
			spawn(apps[match[launchsel]].exec);
		else if (qlen)
			spawn(query);
		endlaunch();
		return;
	case XK_Up:
	case XK_ISO_Left_Tab:
		launchsel = max(launchsel - 1, 0);
		break;
	case XK_Down:
	case XK_Tab:
		launchsel = max(min(launchsel + 1, nmatch - 1), 0);
		break;
	case XK_BackSpace:
		if (qlen)
			query[--qlen] = '\0';
		filter();
		break;
	default:
		/* plain ASCII is enough to find an app or type a command */
		if (n == 1 && buf[0] >= ' ' && buf[0] < 127 &&
		    qlen + 1 < sizeof(query)) {
			query[qlen++] = buf[0];
			query[qlen] = '\0';
			filter();
		}
		break;
	}
	drawlaunch();
}

/* Events */

static void buttonpress(XEvent *e)
{
	XButtonEvent *ev = &e->xbutton;
	struct client *c = find(ev->window);
	struct bar *b;
	int i;

	/* Super+drag is not a tap on Super */
	superalone = 0;
	if (tabbing && tabassist) {
		assistclick(ev->x_root, ev->y_root);
		return;
	}
	if ((b = findbar(ev->window))) {
		if (ev->button == Button1)
			barclick(b, ev->x);
		return;
	}
	for (i = 0; i < 8; i++)
		if (ev->window == grips[i] && sel && ev->button == Button1) {
			resizemouse(sel, i, ev->x_root, ev->y_root);
			return;
		}
	if (!c)
		return;
	if (c != sel)
		focus(c);
	/* let the click through before any drag, or the pointer stays frozen */
	XAllowEvents(dpy, ReplayPointer, CurrentTime);
	if (cleanmask(ev->state) != SUPER)
		return;
	if (ev->button == Button1)
		movemouse(c, ev->x_root, ev->y_root);
	else if (ev->button == Button3)
		resizemouse(c, nearedge(c, ev->x_root, ev->y_root),
			    ev->x_root, ev->y_root);
}

static void clientmessage(XEvent *e)
{
	XClientMessageEvent *ev = &e->xclient;
	struct client *c = find(ev->window);
	long *l = ev->data.l;
	int i, on, maxed = 0;

	if (ev->window == TRAYWIN &&
	    ev->message_type == atom[NET_SYSTEM_TRAY_OPCODE]) {
		if (l[1] == 0)	/* SYSTEM_TRAY_REQUEST_DOCK */
			traydock(l[2]);
		return;
	}
	if (!c)
		return;
	if (ev->message_type == atom[NET_ACTIVE_WINDOW]) {
		focus(c);
	} else if (ev->message_type == atom[NET_CLOSE_WINDOW]) {
		closewin(c);
	} else if (ev->message_type == atom[WM_CHANGE_STATE]) {
		if (l[0] == IconicState)
			minimize(c);
	} else if (ev->message_type == atom[NET_WM_STATE]) {
		/* l[0]: 0 remove, 1 add, 2 toggle; l[1] and l[2]: the states */
		for (i = 1; i <= 2; i++) {
			if ((Atom)l[i] == atom[NET_WM_STATE_FULLSCREEN]) {
				on = l[0] == 1 || (l[0] == 2 && !c->fullscreen);
				if (on != c->fullscreen)
					setfullscreen(c, on);
			}
			maxed |= (Atom)l[i] == atom[NET_WM_STATE_MAXIMIZED_VERT] ||
				 (Atom)l[i] == atom[NET_WM_STATE_MAXIMIZED_HORZ];
		}
		/* the two max states usually come as a pair: act once */
		if (maxed)
			setmax(c, l[0] == 1 || (l[0] == 2 && c->state != MAX));
	} else if (ev->message_type == atom[NET_WM_MOVERESIZE]) {
		if (c != sel)
			focus(c);
		if (l[2] == SZ_MOVE)
			movemouse(c, l[0], l[1]);
		else if (l[2] >= SZ_TL && l[2] < SZ_MOVE)
			resizemouse(c, l[2], l[0], l[1]);
	}
}

static void configurerequest(XEvent *e)
{
	XConfigureRequestEvent *ev = &e->xconfigurerequest;
	struct client *c = find(ev->window);
	XWindowChanges wc;

	if (!c) {
		wc.x = ev->x;
		wc.y = ev->y;
		wc.width = ev->width;
		wc.height = ev->height;
		wc.border_width = ev->border_width;
		wc.sibling = ev->above;
		wc.stack_mode = ev->detail;
		XConfigureWindow(dpy, ev->window, ev->value_mask, &wc);
		return;
	}
	/* floating windows may place themselves; snapped ones stay put */
	if (c->state == FLOAT && !c->fullscreen)
		resize(c, ev->value_mask & CWX ? ev->x : c->x,
		       ev->value_mask & CWY ? ev->y : c->y,
		       ev->value_mask & CWWidth ? ev->width + 2 * c->bw : c->w,
		       ev->value_mask & CWHeight ? ev->height + 2 * c->bw : c->h);
	sendconfig(c);
}

static void keypress(XEvent *e)
{
	XKeyEvent *ev = &e->xkey;
	KeySym sym = XkbKeycodeToKeysym(dpy, ev->keycode, 0, 0);
	unsigned int mod = cleanmask(ev->state);
	size_t i;

	if (tabbing && !tabassist) {
		tabkey(sym, ev->state);
		return;
	}
	if (tabbing && assistkey(sym, ev->state))
		return;
	if (launching) {
		launchkey(ev);
		return;
	}
	/*
	 * Super is grabbed on its own, like Cinnamon's overlay key. Tapped
	 * alone it opens the launcher on release; any other key in between
	 * makes it a Super+key combo instead.
	 */
	if ((sym == XK_Super_L || sym == XK_Super_R) && !mod) {
		superalone = 1;
		return;
	}
	superalone = 0;
	if (sym == XK_w && mod == (ControlMask | ShiftMask)) {
		if (sel)
			closewin(sel);
		return;
	}
	for (i = 0; i < LEN(keys); i++) {
		const struct key *k = &keys[i];

		if (k->sym != sym || k->mod != mod)
			continue;
		switch (k->action) {
		case K_SNAP:
			if (sel)
				snap(sel, k->arg);
			break;
		case K_MOVEMON:
			if (sel)
				movemon(sel, k->arg);
			break;
		case K_CLOSE:
			if (sel)
				closewin(sel);
			break;
		case K_TAB:
			tabstart(k->arg);
			break;
		case K_SPAWN:
			spawn(k->cmd);
			break;
		case K_OSD:
			osdrun(k->cmd, k->arg);
			break;
		case K_QUIT:
			running = 0;
			break;
		}
		return;
	}
}

static void keyrelease(XEvent *e)
{
	KeySym sym = XkbKeycodeToKeysym(dpy, e->xkey.keycode, 0, 0);

	if (tabbing && !tabassist && isaltkey(e->xkey.keycode)) {
		endtab(ntab ? tab[tabsel] : NULL);
	} else if (superalone && (sym == XK_Super_L || sym == XK_Super_R)) {
		superalone = 0;
		launchstart(0);
	} else if (assistpend && (sym == XK_Super_L || sym == XK_Super_R)) {
		struct client *c = assistpend;

		assistpend = NULL;
		if (c->state == LEFT || c->state == RIGHT)
			assiststart(c);
	}
}

static void maprequest(XEvent *e)
{
	XMapRequestEvent *ev = &e->xmaprequest;
	XWindowAttributes wa;
	struct client *c;

	if ((c = find(ev->window))) {
		/* a minimized window mapping itself again wants to be back */
		if (c->minimized)
			focus(c);
		return;
	}
	if (!XGetWindowAttributes(dpy, ev->window, &wa) || wa.override_redirect)
		return;
	adopt(ev->window, &wa);
}

static void propertynotify(XEvent *e)
{
	XPropertyEvent *ev = &e->xproperty;
	struct client *c;
	struct dock *d;
	int on;

	if (ev->state == PropertyDelete)
		return;
	if (ev->window == root) {
		if (ev->atom == XA_WM_NAME)
			bardirty = 1;
	} else if (ev->atom == atom[XEMBED_INFO] &&
		   findtray(ev->window) >= 0) {
		on = traywantsmap(ev->window);
		traymapped(ev->window, on);
		if (on)
			XMapRaised(dpy, ev->window);
		else
			XUnmapWindow(dpy, ev->window);
	} else if ((c = find(ev->window))) {
		/* apps may flip the input hint after mapping */
		if (ev->atom == XA_WM_HINTS) {
			updatenoinput(c);
			return;
		}
		if (ev->atom != XA_WM_NAME && ev->atom != atom[NET_WM_NAME])
			return;
		bardirty = 1;
		if (tabbing)
			drawtab();
	} else if ((d = finddock(ev->window)) &&
		   (ev->atom == atom[NET_WM_STRUT] ||
		    ev->atom == atom[NET_WM_STRUT_PARTIAL])) {
		readstrut(d);
		relayout();
	}
}

static void unmapnotify(XEvent *e)
{
	XUnmapEvent *ev = &e->xunmap;
	struct client *c = find(ev->window);

	if (c) {
		/* a synthetic unmap is a minimized window withdrawing */
		if (ev->send_event || !c->ignoreunmap)
			unmanage(c, 0);
		else
			c->ignoreunmap--;
	} else if (removedock(ev->window)) {
		relayout();
	} else if (!removeabove(ev->window)) {
		traymapped(ev->window, 0);
	}
}

/*
 * A screen plugged in or out: the displays script lays them out. Only a
 * change of connection counts, not the ones the script itself makes.
 */
static void outputchange(XRROutputChangeNotifyEvent *ev)
{
	static struct { RROutput out; int conn; } seen[16];
	int i;

	for (i = 0; i < 16 && seen[i].out && seen[i].out != ev->output; i++)
		;
	if (i < 16) {
		if (seen[i].out == ev->output && seen[i].conn == ev->connection)
			return;
		seen[i].out = ev->output;
		seen[i].conn = ev->connection;
	}
	displays();
}

static void handle(XEvent *ev)
{
	struct client *c;
	int i;

	if (rrbase >= 0 && ev->type == rrbase + RRNotify) {
		if (((XRRNotifyEvent *)ev)->subtype == RRNotify_OutputChange)
			outputchange((XRROutputChangeNotifyEvent *)ev);
		return;
	}
	switch (ev->type) {
	case ButtonPress:
		buttonpress(ev);
		break;
	case ClientMessage:
		clientmessage(ev);
		break;
	case ConfigureRequest:
		configurerequest(ev);
		break;
	case ConfigureNotify:
		/* the screen itself changed: monitors added, moved, resized */
		if (ev->xconfigure.window == root) {
			int ow = sw, oh = sh;

			updatemons();
			keephome(ow, oh);
			placebars();
			relayout();
		} else if (findtray(ev->xconfigure.window) >= 0 &&
			   (ev->xconfigure.width != TRAYSZ ||
			    ev->xconfigure.height != TRAYSZ)) {
			XResizeWindow(dpy, ev->xconfigure.window, TRAYSZ,
				      TRAYSZ);
		}
		break;
	case DestroyNotify:
		if ((c = find(ev->xdestroywindow.window)))
			unmanage(c, 1);
		else if (removedock(ev->xdestroywindow.window))
			relayout();
		else if (!removeabove(ev->xdestroywindow.window))
			trayremove(ev->xdestroywindow.window);
		break;
	case MapNotify:
		traymapped(ev->xmap.window, 1);
		break;
	case ReparentNotify:
		/* an icon taken over by some other tray */
		if (ev->xreparent.parent != TRAYWIN)
			trayremove(ev->xreparent.window);
		break;
	case Expose:
		if (findbar(ev->xexpose.window) && !ev->xexpose.count)
			bardirty = 1;
		if (ev->xexpose.window != tabwin || ev->xexpose.count)
			break;
		if (tabbing)
			drawtab();
		else if (launching)
			drawlaunch();
		break;
	case EnterNotify:
		for (i = 0; i < nbars; i++) {
			if (ev->xcrossing.window == bars[i].trigger)
				showbar(&bars[i], 1);
			if (ev->xcrossing.window == bars[i].trigger ||
			    ev->xcrossing.window == bars[i].win)
				hideat = 0;
		}
		break;
	case LeaveNotify:
		/* into a tray icon is not leaving */
		if (AUTOHIDE && findbar(ev->xcrossing.window) &&
		    ev->xcrossing.detail != NotifyInferior)
			hideat = nowms() + HIDEDELAY;
		break;
	case KeyPress:
		keypress(ev);
		break;
	case KeyRelease:
		keyrelease(ev);
		break;
	case MapRequest:
		maprequest(ev);
		break;
	case MappingNotify:
		XRefreshKeyboardMapping(&ev->xmapping);
		if (ev->xmapping.request != MappingPointer)
			grabkeys();
		break;
	case PropertyNotify:
		propertynotify(ev);
		break;
	case UnmapNotify:
		unmapnotify(ev);
		break;
	}
}

/* Setup */

static int xerror(Display *d, XErrorEvent *ee)
{
	char msg[128];

	/* windows vanish between an event and the reply to it; that is fine */
	if (ee->error_code == BadWindow)
		return 0;
	XGetErrorText(d, ee->error_code, msg, sizeof(msg));
	fprintf(stderr, "nwm: %s (request %d)\n", msg, ee->request_code);
	return 0;
}

static int xerrorstart(Display *d, XErrorEvent *ee)
{
	(void)d;
	(void)ee;
	otherwm = 1;
	return 0;
}

static unsigned long pixel(const char *name)
{
	XColor c, exact;

	if (!XAllocNamedColor(dpy, DefaultColormap(dpy, scr), name, &c, &exact))
		return BlackPixel(dpy, scr);
	return c.pixel;
}

static void xftcolor(XftColor *c, const char *name)
{
	if (!XftColorAllocName(dpy, DefaultVisual(dpy, scr),
			       DefaultColormap(dpy, scr), name, c))
		die("nwm: cannot allocate a colour\n");
}

/* NameWindowPixmap needs Composite 0.2; without it Alt+Tab has no thumbnails. */
static void initcomposite(void)
{
	int ev, err, major = 0, minor = 2;

	if (!XCompositeQueryExtension(dpy, &ev, &err) ||
	    !XCompositeQueryVersion(dpy, &major, &minor) ||
	    (major == 0 && minor < 2) || !XRenderQueryExtension(dpy, &ev, &err))
		return;
	rootfmt = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, scr));
	if (!rootfmt)
		return;
	tabpic = XRenderCreatePicture(dpy, tabwin, rootfmt, 0, NULL);
	composite = 1;
}

static void creategrips(void)
{
	static const unsigned int shapes[8] = {
		[SZ_TL] = XC_top_left_corner, [SZ_T] = XC_top_side,
		[SZ_TR] = XC_top_right_corner, [SZ_R] = XC_right_side,
		[SZ_BR] = XC_bottom_right_corner, [SZ_B] = XC_bottom_side,
		[SZ_BL] = XC_bottom_left_corner, [SZ_L] = XC_left_side,
	};
	XSetWindowAttributes wa = { 0 };
	int i;

	wa.override_redirect = True;
	wa.event_mask = ButtonPressMask;
	for (i = 0; i < 8; i++) {
		wa.cursor = XCreateFontCursor(dpy, shapes[i]);
		grips[i] = XCreateWindow(dpy, root, 0, 0, 1, 1, 0, 0, InputOnly,
					 CopyFromParent, CWOverrideRedirect |
					 CWEventMask | CWCursor, &wa);
	}
}

static void scan(void)
{
	Window d1, d2, *wins = NULL;
	XWindowAttributes wa;
	unsigned int i, n;

	if (!XQueryTree(dpy, root, &d1, &d2, &wins, &n))
		return;
	scanning = 1;
	for (i = 0; i < n; i++) {
		if (!XGetWindowAttributes(dpy, wins[i], &wa) ||
		    wa.override_redirect || wa.map_state != IsViewable)
			continue;
		adopt(wins[i], &wa);
	}
	scanning = 0;
	if (wins)
		XFree(wins);
}

static void setup(void)
{
	XSetWindowAttributes wa = { 0 };
	long v;

	scr = DefaultScreen(dpy);
	root = RootWindow(dpy, scr);

	XSetErrorHandler(xerrorstart);
	XSelectInput(dpy, root, SubstructureRedirectMask |
		     SubstructureNotifyMask | StructureNotifyMask |
		     PropertyChangeMask);
	XSync(dpy, False);
	if (otherwm)
		die("nwm: another window manager is running\n");
	XSetErrorHandler(xerror);
	{
		int err;

		if (XRRQueryExtension(dpy, &rrbase, &err))
			XRRSelectInput(dpy, root, RROutputChangeNotifyMask);
		else
			rrbase = -1;
	}

	XInternAtoms(dpy, atomname, NATOMS, False, atom);
	XkbSetDetectableAutoRepeat(dpy, True, NULL);
	updatemons();

	pix_focus = pixel(COL_FOCUS);
	pix_normal = pixel(COL_NORMAL);
	cur_normal = XCreateFontCursor(dpy, XC_left_ptr);
	cur_move = XCreateFontCursor(dpy, XC_fleur);
	cur_resize = XCreateFontCursor(dpy, XC_sizing);
	XDefineCursor(dpy, root, cur_normal);
	creategrips();

	if (!(font = XftFontOpenName(dpy, scr, FONT)))
		die("nwm: cannot load font\n");
	xftcolor(&xc_bg, COL_TAB_BG);
	xftcolor(&xc_sel, COL_TAB_SEL);
	xftcolor(&xc_fg, COL_TAB_FG);
	xftcolor(&xc_dim, COL_TAB_DIM);
	xftcolor(&xc_box, COL_TAB_BOX);
	wa.override_redirect = True;
	wa.background_pixel = xc_bg.pixel;
	wa.event_mask = ExposureMask;
	tabwin = XCreateWindow(dpy, root, 0, 0, 1, 1, 0, CopyFromParent,
			       InputOutput, CopyFromParent,
			       CWOverrideRedirect | CWBackPixel | CWEventMask,
			       &wa);
	tabdraw = XftDrawCreate(dpy, tabwin, DefaultVisual(dpy, scr),
				DefaultColormap(dpy, scr));
	wa.event_mask = 0;
	osdwin = XCreateWindow(dpy, root, 0, 0, 1, 1, 0, CopyFromParent,
			       InputOutput, CopyFromParent,
			       CWOverrideRedirect | CWBackPixel | CWEventMask,
			       &wa);
	osddraw = XftDrawCreate(dpy, osdwin, DefaultVisual(dpy, scr),
				DefaultColormap(dpy, scr));
	initcomposite();
	placebars();
	if (nbars)
		traystart();

	/* EWMH: who we are and what we support */
	checkwin = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
	XSelectInput(dpy, checkwin, PropertyChangeMask);
	XChangeProperty(dpy, checkwin, atom[NET_SUPPORTING_WM_CHECK],
			XA_WINDOW, 32, PropModeReplace,
			(unsigned char *)&checkwin, 1);
	XChangeProperty(dpy, checkwin, atom[NET_WM_NAME], atom[UTF8_STRING],
			8, PropModeReplace, (unsigned char *)"nwm", 3);
	XChangeProperty(dpy, root, atom[NET_SUPPORTING_WM_CHECK], XA_WINDOW,
			32, PropModeReplace, (unsigned char *)&checkwin, 1);
	XChangeProperty(dpy, root, atom[NET_SUPPORTED], XA_ATOM, 32,
			PropModeReplace, (unsigned char *)&atom[NET_SUPPORTED],
			NATOMS - NET_SUPPORTED);
	v = 1;
	XChangeProperty(dpy, root, atom[NET_NUMBER_OF_DESKTOPS], XA_CARDINAL,
			32, PropModeReplace, (unsigned char *)&v, 1);
	v = 0;
	XChangeProperty(dpy, root, atom[NET_CURRENT_DESKTOP], XA_CARDINAL,
			32, PropModeReplace, (unsigned char *)&v, 1);
	XDeleteProperty(dpy, root, atom[NET_CLIENT_LIST]);
	updateworkarea();

	grabkeys();
	scan();
	if (noauto)
		return;
	displays();
	autostart();
	spawn("f=\"${XDG_CONFIG_HOME:-$HOME/.config}/nwm/autostart\"; "
	      "[ -x \"$f\" ] && exec \"$f\"");
}

/* Leave every window mapped and unframed for whatever runs next. */
static void cleanup(void)
{
	if (tabbing)
		endtab(NULL);
	if (launching)
		endlaunch();
	sel = NULL;
	while (clients) {
		if (clients->minimized)
			XMapWindow(dpy, clients->win);
		XSetWindowBorderWidth(dpy, clients->win, 0);
		unmanage(clients, 0);
	}
	XUngrabKey(dpy, AnyKey, AnyModifier, root);
	if (composite)
		XRenderFreePicture(dpy, tabpic);
	XftDrawDestroy(tabdraw);
	XDestroyWindow(dpy, tabwin);
	/*
	 * The bar is about to go, and its children with it. Icons go back to
	 * the root unmapped; they dock again when the next tray announces
	 * itself.
	 */
	while (ntray--) {
		XUnmapWindow(dpy, tray[ntray].win);
		XReparentWindow(dpy, tray[ntray].win, root, 0, 0);
		XRemoveFromSaveSet(dpy, tray[ntray].win);
	}
	while (nbars--) {
		XftDrawDestroy(bars[nbars].draw);
		XDestroyWindow(dpy, bars[nbars].win);
		if (bars[nbars].trigger)
			XDestroyWindow(dpy, bars[nbars].trigger);
	}
	XDestroyWindow(dpy, checkwin);
	XDeleteProperty(dpy, root, atom[NET_ACTIVE_WINDOW]);
	XDeleteProperty(dpy, root, atom[NET_SUPPORTING_WM_CHECK]);
	XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
	XSync(dpy, False);
	XCloseDisplay(dpy);
}

static void quit(int sig)
{
	running = 0;
	restarting = sig == SIGUSR1;
}

/*
 * Run cmd like spawn(), but keep its output: the level it prints shows in
 * the box once it is done. A newer key press wins over one still running.
 */
static void osdrun(const char *cmd, int what)
{
	struct sigaction sa = { .sa_handler = SIG_DFL };
	int fd[2];

	if (osdfd >= 0)
		close(osdfd);
	osdfd = -1;
	if (pipe(fd) < 0)
		return;
	if (fork() == 0) {
		close(ConnectionNumber(dpy));
		close(fd[0]);
		dup2(fd[1], STDOUT_FILENO);
		setsid();
		sigemptyset(&sa.sa_mask);
		sigaction(SIGCHLD, &sa, NULL);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(fd[1]);
	osdfd = fd[0];
	osdlen = 0;
	osdwhat = what;
}

/* An icon, a bar to 100% and the number, low on the focused monitor. */
static void drawosd(int level, int muted)
{
	struct rect r = mons[sel ? clientmon(sel) : pointermon()];
	int pad = 16, base = (OSDH + font->ascent - font->descent) / 2;
	int bx = pad + 40, bw = OSDW - bx - pad - 56, bh = 6;
	char num[16];

	XMoveResizeWindow(dpy, osdwin, r.x + (r.w - OSDW) / 2,
			  r.y + r.h - OSDH - 4 * BARH, OSDW, OSDH);
	XMapRaised(dpy, osdwin);
	XftDrawRect(osddraw, &xc_bg, 0, 0, OSDW, OSDH);
	drawtext(osddraw, &xc_fg, pad, base, osdwhat == O_LIGHT ? LIGHTICON :
		 muted ? MUTEDICON : SOUND);
	XftDrawRect(osddraw, &xc_box, bx, (OSDH - bh) / 2, bw, bh);
	XftDrawRect(osddraw, muted ? &xc_dim : &xc_sel, bx, (OSDH - bh) / 2,
		    bw * min(max(level, 0), 100) / 100, bh);
	snprintf(num, sizeof(num), "%d%%", level);
	drawright(osddraw, OSDW - pad, base, num);
	osdhideat = nowms() + OSDTIME;
}

/* Take what the command printed; at its end, show the box. */
static void osdread(void)
{
	ssize_t n = read(osdfd, osdbuf + osdlen, sizeof(osdbuf) - 1 - osdlen);

	if (n > 0) {
		osdlen += n;
		if (osdlen < sizeof(osdbuf) - 1)
			return;
	}
	close(osdfd);
	osdfd = -1;
	osdbuf[osdlen] = '\0';
	if (osdlen && osdbuf[0] >= '0' && osdbuf[0] <= '9')
		drawosd(atoi(osdbuf), strstr(osdbuf, " m") != NULL);
}

/*
 * Wait for X, for the clock to need redrawing, or for a bar to be due to
 * hide. 0 when it was a timer.
 */
static int waitevent(void)
{
	int fd = ConnectionNumber(dpy);
	long long ms = (60 - time(NULL) % 60) * 1000LL;
	struct timeval tv;
	fd_set fds;

	if (hideat)
		ms = max(min(ms, hideat - nowms()), 0);
	if (osdhideat)
		ms = max(min(ms, osdhideat - nowms()), 0);
	tv.tv_sec = ms / 1000;
	tv.tv_usec = ms % 1000 * 1000;
	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	if (osdfd >= 0)
		FD_SET(osdfd, &fds);
	XFlush(dpy);
	if (select(max(fd, osdfd) + 1, &fds, NULL, NULL, &tv) > 0) {
		if (osdfd >= 0 && FD_ISSET(osdfd, &fds))
			osdread();
		if (FD_ISSET(fd, &fds))
			return 1;
		return 0;
	}
	if (osdhideat && nowms() >= osdhideat) {
		osdhideat = 0;
		XUnmapWindow(dpy, osdwin);
	}
	if (hideat && nowms() >= hideat) {
		hideat = 0;
		autohide();
	}
	bardirty = 1;
	return 0;
}

int main(int argc, char *argv[])
{
	XEvent ev;
	struct sigaction sa = { .sa_handler = quit };
	char *rargv[] = { argv[0], "-r", NULL };
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			puts("nwm-0.1");
			return 0;
		}
		/*
		 * -n: nested in Xephyr for testing. -r: restarted in place,
		 * the session is already up. Either way, start nothing.
		 */
		if (strcmp(argv[i], "-n") && strcmp(argv[i], "-r"))
			die("usage: nwm [-v] [-n]\n");
		noauto = 1;
	}
	setlocale(LC_CTYPE, "");
	if (!(dpy = XOpenDisplay(NULL)))
		die("nwm: cannot open display\n");
	setenv("XDG_CURRENT_DESKTOP", DESKTOP, 0);
	/* children are reaped by the kernel; spawn() undoes this for them */
	signal(SIGCHLD, SIG_IGN);
	/* TERM is how log out gets here; it ends the session cleanly */
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	/* USR1 restarts in place, e.g. into a freshly installed build */
	sigaction(SIGUSR1, &sa, NULL);
	setup();
	while (running) {
		if (!XPending(dpy)) {
			if (bardirty) {
				drawbar();
				bardirty = 0;
			}
			/* nothing to do: sleep until X talks or the minute turns */
			if (!waitevent())
				continue;
		}
		XNextEvent(dpy, &ev);
		handle(&ev);
	}
	cleanup();
	if (restarting) {
		execvp(argv[0], rargv);
		die("nwm: restart failed\n");
	}
	return 0;
}
