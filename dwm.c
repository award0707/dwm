/* See LICENSE file for copyright and license details.
 *
 * dynamic window manager is designed like any other X client as well. It is
 * driven through handling X events. In contrast to other X clients, a window
 * manager selects for SubstructureRedirectMask on the root window, to receive
 * events about window (dis-)appearance. Only one X connection at a time is
 * allowed to select for this event mask.
 *
 * The event handlers of dwm are organized in an array which is accessed
 * whenever a new event has been fetched. This allows event dispatching
 * in O(1) time.
 *
 * Each child of the root window is called a client, except windows which have
 * set the override_redirect flag. Clients are organized in a linked client
 * list on each monitor, the focus history is remembered through a stack list
 * on each monitor. Each client contains a bit array to indicate the tags of a
 * client.
 *
 * Keys and tagging rules are organized as arrays and defined in config.h.
 *
 * To understand everything else, start reading main().
 */
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif /* XINERAMA */
#include <X11/Xft/Xft.h>
#include <X11/Xlib-xcb.h>
#include <xcb/res.h>
#ifdef __OpenBSD__
#include <sys/sysctl.h>
#include <kvm.h>
#endif /* __OpenBSD */

#include "drw.h"
#include "util.h"

/* macros */
#define BUTTONMASK              (ButtonPressMask|ButtonReleaseMask)
#define CLEANMASK(mask)         (mask & ~(numlockmask|LockMask) & (ShiftMask|ControlMask|Mod1Mask|Mod2Mask|Mod3Mask|Mod4Mask|Mod5Mask))
#define INTERSECT(x,y,w,h,m)    (MAX(0, MIN((x)+(w),(m)->wx+(m)->ww) - MAX((x),(m)->wx)) \
                               * MAX(0, MIN((y)+(h),(m)->wy+(m)->wh) - MAX((y),(m)->wy)))
#define ISVISIBLEONTAG(C, T)    ((C->tags & T))
#define ISVISIBLE(C)            ISVISIBLEONTAG(C, C->mon->tagset[C->mon->seltags])
#define LENGTH(X)               (sizeof X / sizeof X[0])
#define MOUSEMASK               (BUTTONMASK|PointerMotionMask)
#define WIDTH(X)                ((X)->w + 2 * (X)->bw)
#define HEIGHT(X)               ((X)->h + 2 * (X)->bw)
#define TAGMASK                 ((1 << LENGTH(tags)) - 1)
#define TEXTW(X)                (drw_fontset_getwidth(drw, (X)) + lrpad)

#define GAP_TOGGLE 100
#define GAP_RESET  0

#define XRDB_LOAD_COLOR(R,V)    if (XrmGetResource(xrdb, R, NULL, &type, &value) == True) { \
                                  if (value.addr != NULL && strnlen(value.addr, 8) == 7 && value.addr[0] == '#') { \
                                    int i = 1; \
                                    for (; i <= 6; i++) { \
                                      if (value.addr[i] < 48) break; \
                                      if (value.addr[i] > 57 && value.addr[i] < 65) break; \
                                      if (value.addr[i] > 70 && value.addr[i] < 97) break; \
                                      if (value.addr[i] > 102) break; \
                                    } \
                                    if (i == 7) { \
                                      strncpy(V, value.addr, 7); \
                                      V[7] = '\0'; \
                                    } \
                                  } \
                                }

#define MWM_HINTS_FLAGS_FIELD       0
#define MWM_HINTS_DECORATIONS_FIELD 2
#define MWM_HINTS_DECORATIONS       (1 << 1)
#define MWM_DECOR_ALL               (1 << 0)
#define MWM_DECOR_BORDER            (1 << 1)
#define MWM_DECOR_TITLE             (1 << 3)

#define SYSTEM_TRAY_REQUEST_DOCK    0
/* XEMBED messages */
#define XEMBED_EMBEDDED_NOTIFY      0
#define XEMBED_WINDOW_ACTIVATE      1
#define XEMBED_FOCUS_IN             4
#define XEMBED_MODALITY_ON         10
#define XEMBED_MAPPED              (1 << 0)
#define XEMBED_WINDOW_ACTIVATE      1
#define XEMBED_WINDOW_DEACTIVATE    2
#define VERSION_MAJOR               0
#define VERSION_MINOR               0
#define XEMBED_EMBEDDED_VERSION (VERSION_MAJOR << 16) | VERSION_MINOR

/* enums */
enum { CurNormal, CurResize, CurMove, CurLast }; /* cursor */
enum { SchemeNorm, SchemeSel, SchemeRev, /* color schemes */
       Red, Yellow, Orange, Green, Cyan, Blue, Violet, Magenta, Urgent, /* 3-11 */
       SchemeBorder, SchemeBorderSel };
enum { NetSupported, NetWMName, NetWMState, NetWMCheck,
       NetSystemTray, NetSystemTrayOP, NetSystemTrayOrientation, NetSystemTrayOrientationHorz,
       NetWMFullscreen, NetActiveWindow, NetWMWindowType,
       NetWMWindowTypeDialog, NetClientList, NetClientInfo, NetLast }; /* EWMH atoms */
enum { Manager, Xembed, XembedInfo, XLast }; /* Xembed atoms */
enum { WMProtocols, WMDelete, WMState, WMTakeFocus, WMLast }; /* default atoms */
enum { ClkTagBar, ClkLtSymbol, ClkStatusText, ClkWinTitle,
       ClkClientWin, ClkRootWin, ClkLast }; /* clicks */

typedef struct TagState TagState;
struct TagState {
	int selected;
	int occupied;
	int urgent;
};

typedef struct ClientState ClientState;
struct ClientState {
	int isfixed, isfloating, isurgent, neverfocus, oldstate, isfullscreen;
};

typedef union {
	long i;
	unsigned long ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int click;
	unsigned int mask;
	unsigned int button;
	void (*func)(const Arg *arg);
	const Arg arg;
} Button;

typedef struct Monitor Monitor;
typedef struct Client Client;
struct Client {
	char name[256];
	float mina, maxa;
	int x, y, w, h;
	int oldx, oldy, oldw, oldh;
	int basew, baseh, incw, inch, maxw, maxh, minw, minh;
	int bw, oldbw;
	unsigned int tags;
	int issteam;
	int isfixed, isfloating, isurgent, neverfocus, oldstate, isfullscreen, isterminal, noswallow;
	pid_t pid;
	Client *next;
	Client *snext;
	Client *swallowing;
	Monitor *mon;
	Window win;
	ClientState prevstate;
};

typedef struct {
	unsigned int mod;
	KeySym keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	const char *symbol;
	void (*arrange)(Monitor *);
} Layout;

typedef struct Pertag Pertag;

struct Monitor {
	char ltsymbol[16];
	char lastltsymbol[16];
	float mfact;
	int nmaster;
	int num;
	int by;               /* bar geometry */
	int mx, my, mw, mh;   /* screen size */
	int wx, wy, ww, wh;   /* window area  */
	unsigned int borderpx;
	unsigned int seltags;
	unsigned int sellt;
	unsigned int tagset[2];
	TagState tagstate;
	int showbar;
	int topbar;
	Client *clients;
	Client *sel;
	Client *lastsel;
	Client *stack;
	Monitor *next;
	Window barwin;
	const Layout *lt[2];
	Pertag *pertag;
	const Layout *lastlt;
	Client *tagmarked[32];
};

typedef struct {
	const char *class;
	const char *instance;
	const char *title;
	unsigned int tags;
	int isfloating;
	int isterminal;
	int noswallow;
	int monitor;
} Rule;

typedef struct Systray   Systray;
struct Systray {
	Window win;
	Client *icons;
};

/* function declarations */
static void applyrules(Client *c);
static int applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact);
static void arrange(Monitor *m);
static void arrangemon(Monitor *m);
static void attach(Client *c);
static void attachaside(Client *c);
static void attachstack(Client *c);
static void buttonpress(XEvent *e);
static void checkotherwm(void);
static void cleanup(void);
static void cleanupmon(Monitor *mon);
static void clientmessage(XEvent *e);
static void configure(Client *c);
static void configurenotify(XEvent *e);
static void configurerequest(XEvent *e);
static Monitor *createmon(void);
static void cyclelayout(const Arg *arg);
static void deck(Monitor *m);
static void destroynotify(XEvent *e);
static void detach(Client *c);
static void detachstack(Client *c);
static Monitor *dirtomon(int dir);
static void drawbar(Monitor *m);
static void drawbars(void);
static int drawstatusbar(Monitor *m, int bh, char* text);
static void enternotify(XEvent *e);
static void expose(XEvent *e);
static void focus(Client *c);
static void focusin(XEvent *e);
static void focusmon(const Arg *arg);
static void focusstack(const Arg *arg);
static void focusmaster(const Arg *arg);
static Atom getatomprop(Client *c, Atom prop);
static int getrootptr(int *x, int *y);
static long getstate(Window w);
static pid_t getstatusbarpid();
static unsigned int getsystraywidth();
static int gettextprop(Window w, Atom atom, char *text, unsigned int size);
static void grabbuttons(Client *c, int focused);
static void grabkeys(void);
static void gridstack(Monitor *m);
static int handlexevent(struct epoll_event *ev);
static void incnmaster(const Arg *arg);
static void keypress(XEvent *e);
static void killclient(const Arg *arg);
static void loadxrdb(void);
static void manage(Window w, XWindowAttributes *wa);
static void mappingnotify(XEvent *e);
static void maprequest(XEvent *e);
static void monocle(Monitor *m);
static void motionnotify(XEvent *e);
static void movemouse(const Arg *arg);
static Client *nexttagged(Client *c);
static Client *nexttiled(Client *c);
static void pop(Client *c);
static Client *prevtiled(Client *c);
static void propertynotify(XEvent *e);
static void pushdown(const Arg *arg);
static void pushup(const Arg *arg);
static void quit(const Arg *arg);
static Monitor *recttomon(int x, int y, int w, int h);
static void removesystrayicon(Client *i);
static void resize(Client *c, int x, int y, int w, int h, int interact);
static void resizebarwin(Monitor *m);
static void resizeclient(Client *c, int x, int y, int w, int h);
static void resizemouse(const Arg *arg);
static void resizerequest(XEvent *e);
static void restack(Monitor *m);
static void run(void);
static void scan(void);
static int sendevent(Window w, Atom proto, int m, long d0, long d1, long d2, long d3, long d4);
static void setborderpx(void);
static void sendmon(Client *c, Monitor *m);
static void setclientstate(Client *c, long state);
static void setclienttagprop(Client *c);
static void setfocus(Client *c);
static void setfullscreen(Client *c, int fullscreen);
static void setgaps(const Arg *arg);
static void setgappx(const Arg *arg);
static void setlayout(const Arg *arg);
static void setlayoutsafe(const Arg *arg);
static void setmfact(const Arg *arg);
static void setup(void);
static void setupepoll(void);
static void seturgent(Client *c, int urg);
static void showhide(Client *c);
static void sigchld(int unused);
static void sigstatusbar(const Arg *arg);
static void sighup(int unused);
static void sigterm(int unused);
static void spawn(const Arg *arg);
static Monitor *systraytomon(Monitor *m);
static void switchcol(const Arg *arg);
static void tag(const Arg *arg);
static void tagmon(const Arg *arg);
static void tile(Monitor *m);
static void tilewide(Monitor *m);
static void togglebar(const Arg *arg);
static void togglefloating(const Arg *arg);
static void togglefullscreen(const Arg *arg);
static void togglescratch(const Arg *arg);
static void toggletag(const Arg *arg);
static void toggleview(const Arg *arg);
static void unfocus(Client *c, int setfocus);
static void unmanage(Client *c, int destroyed);
static void unmapnotify(XEvent *e);
static void updatebarpos(Monitor *m);
static void updatebars(void);
static void updateclientlist(void);
static int updategeom(void);
static void updatemotifhints(Client *c);
static void updatenumlockmask(void);
static void updatesizehints(Client *c);
static void updatestatus(void);
static void updatesystray(void);
static void updatesystrayicongeom(Client *i, int w, int h);
static void updatesystrayiconstate(Client *i, XPropertyEvent *ev);
static void updatetitle(Client *c);
static void updatewindowtype(Client *c);
static void updatewmhints(Client *c);
static void view(const Arg *arg);
static Client *wintoclient(Window w);
static Monitor *wintomon(Window w);
static Client *wintosystrayicon(Window w);
static int xerror(Display *dpy, XErrorEvent *ee);
static int xerrordummy(Display *dpy, XErrorEvent *ee);
static int xerrorstart(Display *dpy, XErrorEvent *ee);
static void xrdb(const Arg *arg);
static void zoom(const Arg *arg);
static void autostart_exec(void);

static pid_t getparentprocess(pid_t p);
static int isdescprocess(pid_t p, pid_t c);
static Client *swallowingclient(Window w);
static Client *termforwin(const Client *c);
static pid_t winpid(Window w);
static void centeredmaster(Monitor *m);
static void centeredfloatingmaster(Monitor *m);

/* variables */
static Systray *systray = NULL;
static const char broken[] = "broken";
static char stext[1024];
static int statussig;
static int statusw;
static pid_t statuspid = -1;
static int screen;
static int sw, sh;           /* X display screen geometry width, height */
static int bh, blw = 0;      /* bar geometry */
static int lrpad;            /* sum of left and right padding for text */
static int (*xerrorxlib)(Display *, XErrorEvent *);
static unsigned int numlockmask = 0;
static void (*handler[LASTEvent]) (XEvent *) = {
	[ButtonPress] = buttonpress,
	[ClientMessage] = clientmessage,
	[ConfigureRequest] = configurerequest,
	[ConfigureNotify] = configurenotify,
	[DestroyNotify] = destroynotify,
	[EnterNotify] = enternotify,
	[Expose] = expose,
	[FocusIn] = focusin,
	[KeyPress] = keypress,
	[MappingNotify] = mappingnotify,
	[MapRequest] = maprequest,
	[MotionNotify] = motionnotify,
	[PropertyNotify] = propertynotify,
	[ResizeRequest] = resizerequest,
	[UnmapNotify] = unmapnotify
};
static int restart = 0;
static Atom wmatom[WMLast], netatom[NetLast], motifatom, xatom[XLast];
static int epoll_fd;
static int dpy_fd;
static int running = 1;
static Cur *cursor[CurLast];
static Clr **scheme;
static Display *dpy;
static Drw *drw;
static Monitor *mons, *selmon, *lastselmon;
static Window root, wmcheckwin;

#include "ipc.h"
static xcb_connection_t *xcon;

/* configuration, allows nested code to access above variables */
#include "config.h"

struct Pertag {
	unsigned int curtag, prevtag; /* current and previous tag */
	int nmasters[LENGTH(tags) + 1]; /* number of windows in master area */
	float mfacts[LENGTH(tags) + 1]; /* mfacts per tag */
	unsigned int sellts[LENGTH(tags) + 1]; /* selected layouts */
	const Layout *ltidxs[LENGTH(tags) + 1][2]; /* matrix of tags and layouts indexes  */
	int showbars[LENGTH(tags) + 1]; /* display bar for the current tag */

	int drawwithgaps[LENGTH(tags) + 1]; /* gaps toggle for each tag */
	int gappx[LENGTH(tags) + 1]; /* gaps for each tag */
};
#ifdef VERSION
#include "IPCClient.c"
#include "yajl_dumps.c"
#include "ipc.c"
#endif
static unsigned int scratchtag = 1 << LENGTH(tags);

/* compile-time check if all tags fit into an unsigned int bit array. */
struct NumTags { char limitexceeded[LENGTH(tags) > 31 ? -1 : 1]; };

/* dwm will keep pid's of processes from autostart array and kill them at quit */
static pid_t *autostart_pids;
static size_t autostart_len;

/* execute command from autostart array */
static void
autostart_exec() {
	const char *const *p;
	size_t i = 0;

	/* count entries */
	for (p = autostart; *p; autostart_len++, p++)
		while (*++p);

	autostart_pids = malloc(autostart_len * sizeof(pid_t));
	for (p = autostart; *p; i++, p++) {
		if ((autostart_pids[i] = fork()) == 0) {
			setsid();
			execvp(*p, (char *const *)p);
			fprintf(stderr, "dwm: execvp %s\n", *p);
			perror(" failed");
			_exit(EXIT_FAILURE);
		}
		/* skip arguments */
		while (*++p);
	}
}

/* function implementations */
void
applyrules(Client *c)
{
	const char *class, *instance;
	unsigned int i;
	const Rule *r;
	Monitor *m;
	XClassHint ch = { NULL, NULL };

	/* rule matching */
	c->isfloating = 0;
	c->tags = 0;
	XGetClassHint(dpy, c->win, &ch);
	class    = ch.res_class ? ch.res_class : broken;
	instance = ch.res_name  ? ch.res_name  : broken;

	if (strstr(class, "Steam") || strstr(class, "steam_app_"))
		c->issteam = 1;

	for (i = 0; i < LENGTH(rules); i++) {
		r = &rules[i];
		if ((!r->title || strstr(c->name, r->title))
		&& (!r->class || strstr(class, r->class))
		&& (!r->instance || strstr(instance, r->instance)))
		{
			c->isterminal = r->isterminal;
			c->noswallow  = r->noswallow;
			c->isfloating = r->isfloating;
			c->tags |= r->tags;
			for (m = mons; m && m->num != r->monitor; m = m->next);
			if (m)
				c->mon = m;
		}
	}
	if (ch.res_class)
		XFree(ch.res_class);
	if (ch.res_name)
		XFree(ch.res_name);
	c->tags = c->tags & TAGMASK ? c->tags & TAGMASK : c->mon->tagset[c->mon->seltags];
}

int
applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact)
{
	int baseismin;
	Monitor *m = c->mon;

	/* set minimum possible */
	*w = MAX(1, *w);
	*h = MAX(1, *h);
	if (interact) {
		if (*x > sw)
			*x = sw - WIDTH(c);
		if (*y > sh)
			*y = sh - HEIGHT(c);
		if (*x + *w + 2 * c->bw < 0)
			*x = 0;
		if (*y + *h + 2 * c->bw < 0)
			*y = 0;
	} else {
		if (*x >= m->wx + m->ww)
			*x = m->wx + m->ww - WIDTH(c);
		if (*y >= m->wy + m->wh)
			*y = m->wy + m->wh - HEIGHT(c);
		if (*x + *w + 2 * c->bw <= m->wx)
			*x = m->wx;
		if (*y + *h + 2 * c->bw <= m->wy)
			*y = m->wy;
	}
	if (*h < bh)
		*h = bh;
	if (*w < bh)
		*w = bh;
	if (resizehints || c->isfloating || !c->mon->lt[c->mon->sellt]->arrange) {
		/* see last two sentences in ICCCM 4.1.2.3 */
		baseismin = c->basew == c->minw && c->baseh == c->minh;
		if (!baseismin) { /* temporarily remove base dimensions */
			*w -= c->basew;
			*h -= c->baseh;
		}
		/* adjust for aspect limits */
		if (c->mina > 0 && c->maxa > 0) {
			if (c->maxa < (float)*w / *h)
				*w = *h * c->maxa + 0.5;
			else if (c->mina < (float)*h / *w)
				*h = *w * c->mina + 0.5;
		}
		if (baseismin) { /* increment calculation requires this */
			*w -= c->basew;
			*h -= c->baseh;
		}
		/* adjust for increment value */
		if (c->incw)
			*w -= *w % c->incw;
		if (c->inch)
			*h -= *h % c->inch;
		/* restore base dimensions */
		*w = MAX(*w + c->basew, c->minw);
		*h = MAX(*h + c->baseh, c->minh);
		if (c->maxw)
			*w = MIN(*w, c->maxw);
		if (c->maxh)
			*h = MIN(*h, c->maxh);
	}
	return *x != c->x || *y != c->y || *w != c->w || *h != c->h;
}

void
arrange(Monitor *m)
{
	if (m)
		showhide(m->stack);
	else for (m = mons; m; m = m->next)
		showhide(m->stack);
	if (m) {
		arrangemon(m);
		restack(m);
	} else for (m = mons; m; m = m->next)
		arrangemon(m);
}

void
arrangemon(Monitor *m)
{
	strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, sizeof m->ltsymbol);
	if (m->lt[m->sellt]->arrange)
		m->lt[m->sellt]->arrange(m);
}

void
attach(Client *c)
{
	c->next = c->mon->clients;
	c->mon->clients = c;
}

void
attachaside(Client *c) {
	Client *at = nexttagged(c);
	if(!at) {
		attach(c);
		return;
	}
	c->next = at->next;
	at->next = c;
}

void
attachstack(Client *c)
{
	c->snext = c->mon->stack;
	c->mon->stack = c;
}

void
swallow(Client *p, Client *c)
{

	if (c->noswallow || c->isterminal)
		return;
	if (c->noswallow && !swallowfloating && c->isfloating)
		return;

	detach(c);
	detachstack(c);

	setclientstate(c, WithdrawnState);
	XUnmapWindow(dpy, p->win);

	p->swallowing = c;
	c->mon = p->mon;

	Window w = p->win;
	p->win = c->win;
	c->win = w;
	updatetitle(p);
	XMoveResizeWindow(dpy, p->win, p->x, p->y, p->w, p->h);
	arrange(p->mon);
	configure(p);
	updateclientlist();
}

void
unswallow(Client *c)
{
	c->win = c->swallowing->win;

	free(c->swallowing);
	c->swallowing = NULL;

	/* unfullscreen the client */
	setfullscreen(c, 0);
	updatetitle(c);
	arrange(c->mon);
	XMapWindow(dpy, c->win);
	XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
	setclientstate(c, NormalState);
	focus(NULL);
	arrange(c->mon);
}

void
buttonpress(XEvent *e)
{
	unsigned int i, x, click, stw;
	Arg arg = {0};
	Client *c;
	Monitor *m;
	XButtonPressedEvent *ev = &e->xbutton;

	click = ClkRootWin;
	/* focus monitor if necessary */
	if ((m = wintomon(ev->window)) && m != selmon) {
		unfocus(selmon->sel, 1);
		selmon = m;
		focus(NULL);
	}
	if(showsystray && m == systraytomon(m) && !systrayonleft)
		stw = getsystraywidth();
	else
		stw = 0;
	if (ev->window == selmon->barwin) {
		i = x = 0;
		do
			x += TEXTW(tags[i]);
		while (ev->x >= x && ++i < LENGTH(tags));
		if (i < LENGTH(tags)) {
			click = ClkTagBar;
			arg.ui = 1 << i;
		} else if (ev->x < x + blw)
			click = ClkLtSymbol;
		else if (ev->x > selmon->ww - statusw - stw) {
			x = selmon->ww - statusw - stw;
			click = ClkStatusText;

			char *text, *s, ch;
			statussig = 0;
			for (text = s = stext; *s && x <= ev->x; s++) {
				if ((unsigned char)(*s) < ' ') {
					ch = *s;
					*s = '\0';
					x += TEXTW(text) - lrpad;
					*s = ch;
					text = s + 1;
					if (x >= ev->x)
						break;
					statussig = ch;
				} else if (*s == '^') {
					*s = '\0';
					x += TEXTW(text) - lrpad;
					*s = '^';
					if (*(++s) == 'f')
						x += atoi(++s);
					while (*(s++) != '^');
					text = s;
					s--;
				}
			}
		} else
			click = ClkWinTitle;
	} else if ((c = wintoclient(ev->window))) {
		focus(c);
		restack(selmon);
		XAllowEvents(dpy, ReplayPointer, CurrentTime);
		click = ClkClientWin;
	}
	for (i = 0; i < LENGTH(buttons); i++)
		if (click == buttons[i].click && buttons[i].func && buttons[i].button == ev->button
		&& CLEANMASK(buttons[i].mask) == CLEANMASK(ev->state))
			buttons[i].func(click == ClkTagBar && buttons[i].arg.i == 0 ? &arg : &buttons[i].arg);
}

void
checkotherwm(void)
{
	xerrorxlib = XSetErrorHandler(xerrorstart);
	/* this causes an error if some other window manager is running */
	XSelectInput(dpy, DefaultRootWindow(dpy), SubstructureRedirectMask);
	XSync(dpy, False);
	XSetErrorHandler(xerror);
	XSync(dpy, False);
}

void
cleanup(void)
{
	Arg a = {.ui = ~0};
	Layout foo = { "", NULL };
	Monitor *m;
	size_t i;

	view(&a);
	selmon->lt[selmon->sellt] = &foo;
	for (m = mons; m; m = m->next)
		while (m->stack)
			unmanage(m->stack, 0);
	XUngrabKey(dpy, AnyKey, AnyModifier, root);
	while (mons)
		cleanupmon(mons);

	if (showsystray) {
		XUnmapWindow(dpy, systray->win);
		XDestroyWindow(dpy, systray->win);
		free(systray);
	}

	for (i = 0; i < CurLast; i++)
		drw_cur_free(drw, cursor[i]);
	for (i = 0; i < LENGTH(colors) + 1; i++)
		free(scheme[i]);
	XDestroyWindow(dpy, wmcheckwin);
	drw_free(drw);
	XSync(dpy, False);
	XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
	XDeleteProperty(dpy, root, netatom[NetActiveWindow]);

	ipc_cleanup();

	if (close(epoll_fd) < 0) {
			fprintf(stderr, "Failed to close epoll file descriptor\n");
	}
}

void
cleanupmon(Monitor *mon)
{
	Monitor *m;

	if (mon == mons)
		mons = mons->next;
	else {
		for (m = mons; m && m->next != mon; m = m->next);
		m->next = mon->next;
	}
	XUnmapWindow(dpy, mon->barwin);
	XDestroyWindow(dpy, mon->barwin);
	free(mon);
}

void
clientmessage(XEvent *e)
{
	XWindowAttributes wa;
	XSetWindowAttributes swa;
	XClientMessageEvent *cme = &e->xclient;
	Client *c = wintoclient(cme->window);

	if (showsystray && cme->window == systray->win && cme->message_type == netatom[NetSystemTrayOP]) {
		/* add systray icons */
		if (cme->data.l[1] == SYSTEM_TRAY_REQUEST_DOCK) {
			if (!(c = (Client *)calloc(1, sizeof(Client))))
				die("fatal: could not malloc() %u bytes\n", sizeof(Client));
			if (!(c->win = cme->data.l[2])) {
				free(c);
				return;
			}
			c->mon = selmon;
			c->next = systray->icons;
			systray->icons = c;
			if (!XGetWindowAttributes(dpy, c->win, &wa)) {
				/* use sane defaults */
				wa.width = bh;
				wa.height = bh;
				wa.border_width = 0;
			}
			c->x = c->oldx = c->y = c->oldy = 0;
			c->w = c->oldw = wa.width;
			c->h = c->oldh = wa.height;
			c->oldbw = wa.border_width;
			c->bw = 0;
			c->isfloating = True;
			/* reuse tags field as mapped status */
			c->tags = 1;
			updatesizehints(c);
			updatesystrayicongeom(c, wa.width, wa.height);
			XAddToSaveSet(dpy, c->win);
			XSelectInput(dpy, c->win, StructureNotifyMask | PropertyChangeMask | ResizeRedirectMask);
			XReparentWindow(dpy, c->win, systray->win, 0, 0);
			/* use parents background color */
			swa.background_pixel  = scheme[SchemeNorm][ColBg].pixel;
			XChangeWindowAttributes(dpy, c->win, CWBackPixel, &swa);
			sendevent(c->win, netatom[Xembed], StructureNotifyMask, CurrentTime, XEMBED_EMBEDDED_NOTIFY, 0 , systray->win, XEMBED_EMBEDDED_VERSION);
			/* FIXME not sure if I have to send these events, too */
			sendevent(c->win, netatom[Xembed], StructureNotifyMask, CurrentTime, XEMBED_FOCUS_IN, 0 , systray->win, XEMBED_EMBEDDED_VERSION);
			sendevent(c->win, netatom[Xembed], StructureNotifyMask, CurrentTime, XEMBED_WINDOW_ACTIVATE, 0 , systray->win, XEMBED_EMBEDDED_VERSION);
			sendevent(c->win, netatom[Xembed], StructureNotifyMask, CurrentTime, XEMBED_MODALITY_ON, 0 , systray->win, XEMBED_EMBEDDED_VERSION);
			XSync(dpy, False);
			resizebarwin(selmon);
			updatesystray();
			setclientstate(c, NormalState);
		}
		return;
	}

	if (!c)
		return;
	if (cme->message_type == netatom[NetWMState]) {
		if (cme->data.l[1] == netatom[NetWMFullscreen]
		|| cme->data.l[2] == netatom[NetWMFullscreen])
			setfullscreen(c, (cme->data.l[0] == 1 /* _NET_WM_STATE_ADD    */
				|| (cme->data.l[0] == 2 /* _NET_WM_STATE_TOGGLE */ && !c->isfullscreen)));
	} else if (cme->message_type == netatom[NetActiveWindow]) {
		if (c != selmon->sel && !c->isurgent)
			seturgent(c, 1);
	}
}

void
configure(Client *c)
{
	XConfigureEvent ce;

	ce.type = ConfigureNotify;
	ce.display = dpy;
	ce.event = c->win;
	ce.window = c->win;
	ce.x = c->x;
	ce.y = c->y;
	ce.width = c->w;
	ce.height = c->h;
	ce.border_width = c->bw;
	ce.above = None;
	ce.override_redirect = False;
	XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

void
configurenotify(XEvent *e)
{
	Monitor *m;
	Client *c;
	XConfigureEvent *ev = &e->xconfigure;
	int dirty;

	/* TODO: updategeom handling sucks, needs to be simplified */
	if (ev->window == root) {
		dirty = (sw != ev->width || sh != ev->height);
		sw = ev->width;
		sh = ev->height;
		if (updategeom() || dirty) {
			drw_resize(drw, sw, bh);
			updatebars();
			for (m = mons; m; m = m->next) {
				for (c = m->clients; c; c = c->next)
					if (c->isfullscreen)
						resizeclient(c, m->mx, m->my, m->mw, m->mh);
				resizebarwin(m);
			}
			focus(NULL);
			arrange(NULL);
		}
	}
}

void
configurerequest(XEvent *e)
{
	Client *c;
	Monitor *m;
	XConfigureRequestEvent *ev = &e->xconfigurerequest;
	XWindowChanges wc;

	if ((c = wintoclient(ev->window))) {
		if (ev->value_mask & CWBorderWidth)
			c->bw = ev->border_width;
		else if (c->isfloating || !selmon->lt[selmon->sellt]->arrange) {
			m = c->mon;
			if (!c->issteam) {
				if (ev->value_mask & CWX) {
					c->oldx = c->x;
					c->x = m->mx + ev->x;
				}
				if (ev->value_mask & CWY) {
					c->oldy = c->y;
					c->y = m->my + ev->y;
				}
			}
			if (ev->value_mask & CWWidth) {
				c->oldw = c->w;
				c->w = ev->width;
			}
			if (ev->value_mask & CWHeight) {
				c->oldh = c->h;
				c->h = ev->height;
			}
			if ((c->x + c->w) > m->mx + m->mw && c->isfloating)
				c->x = m->mx + (m->mw / 2 - WIDTH(c) / 2); /* center in x direction */
			if ((c->y + c->h) > m->my + m->mh && c->isfloating)
				c->y = m->my + (m->mh / 2 - HEIGHT(c) / 2); /* center in y direction */
			if ((ev->value_mask & (CWX|CWY)) && !(ev->value_mask & (CWWidth|CWHeight)))
				configure(c);
			if (ISVISIBLE(c))
				XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
		} else
			configure(c);
	} else {
		wc.x = ev->x;
		wc.y = ev->y;
		wc.width = ev->width;
		wc.height = ev->height;
		wc.border_width = ev->border_width;
		wc.sibling = ev->above;
		wc.stack_mode = ev->detail;
		XConfigureWindow(dpy, ev->window, ev->value_mask, &wc);
	}
	XSync(dpy, False);
}

Monitor *
createmon(void)
{
	Monitor *m;
	unsigned int i;

	m = ecalloc(1, sizeof(Monitor));
	m->tagset[0] = m->tagset[1] = 1;
	m->mfact = mfact;
	m->nmaster = nmaster;
	m->showbar = showbar;
	m->topbar = topbar;
	m->borderpx = borderpx;	
	m->lt[0] = &layouts[0];
	m->lt[1] = &layouts[1 % LENGTH(layouts)];
	strncpy(m->ltsymbol, layouts[0].symbol, sizeof m->ltsymbol);
	m->pertag = ecalloc(1, sizeof(Pertag));
	m->pertag->curtag = m->pertag->prevtag = 1;

	for (i = 0; i <= LENGTH(tags); i++) {
		m->pertag->nmasters[i] = m->nmaster;
		m->pertag->mfacts[i] = m->mfact;

		m->pertag->ltidxs[i][0] = m->lt[0];
		m->pertag->ltidxs[i][1] = m->lt[1];
		m->pertag->sellts[i] = m->sellt;

		m->pertag->showbars[i] = m->showbar;

		if (i > 0) {
			m->pertag->drawwithgaps[i] = startwithgaps[(i - 1) % LENGTH(gappx)];
			m->pertag->gappx[i] = gappx[(i - 1) % LENGTH(gappx)];
		}
	}
	m->pertag->drawwithgaps[0] = startwithgaps[0];
	m->pertag->gappx[0] = gappx[0];

	return m;
}

void
cyclelayout(const Arg *arg) {
	Layout *l;
	for(l = (Layout *)layouts; l != selmon->lt[selmon->sellt]; l++);
	if(arg->i > 0) {
		if(l->symbol && (l + 1)->symbol)
			setlayout(&((Arg) { .v = (l + 1) }));
		else
			setlayout(&((Arg) { .v = layouts }));
	} else {
		if(l != layouts && (l - 1)->symbol)
			setlayout(&((Arg) { .v = (l - 1) }));
		else
			setlayout(&((Arg) { .v = &layouts[LENGTH(layouts) - 2] }));
	}
}

__attribute__((unused))
void
deck(Monitor *m)
{
	unsigned int i, n, h, mw, my;
	Client *c;

	for(n = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), n++);
	if(n == 0)
		return;

	if(n > m->nmaster) {
		mw = m->nmaster ? m->ww * m->mfact : 0;
		snprintf(m->ltsymbol, sizeof m->ltsymbol, "[][%d]", n - m->nmaster);
	}
	else
		mw = m->ww;
	for(i = my = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), i++)
		if(i < m->nmaster) {
			h = (m->wh - my) / (MIN(n, m->nmaster) - i);
			resize(c, m->wx, m->wy + my, mw - (2*c->bw), h - (2*c->bw), False);
			my += HEIGHT(c);
		}
		else
			resize(c, m->wx + mw, m->wy, m->ww - mw - (2*c->bw), m->wh - (2*c->bw), False);
}

void
destroynotify(XEvent *e)
{
	Client *c;
	XDestroyWindowEvent *ev = &e->xdestroywindow;

	if ((c = wintoclient(ev->window)))
		unmanage(c, 1);
	else if ((c = wintosystrayicon(ev->window))) {
		removesystrayicon(c);
		resizebarwin(selmon);
		updatesystray();
	}
	else if ((c = swallowingclient(ev->window)))
		unmanage(c->swallowing, 1);
}

void
detach(Client *c)
{
	Client **tc;

	for (int i=1; i<LENGTH(tags); ++i)
		if (c == c->mon->tagmarked[i])
			c->mon->tagmarked[i] = NULL;

	for (tc = &c->mon->clients; *tc && *tc != c; tc = &(*tc)->next);
	*tc = c->next;
}

void
detachstack(Client *c)
{
	Client **tc, *t;

	for (tc = &c->mon->stack; *tc && *tc != c; tc = &(*tc)->snext);
	*tc = c->snext;

	if (c == c->mon->sel) {
		for (t = c->mon->stack; t && !ISVISIBLE(t); t = t->snext);
		c->mon->sel = t;
	}
}

Monitor *
dirtomon(int dir)
{
	Monitor *m = NULL;

	if (dir > 0) {
		if (!(m = selmon->next))
			m = mons;
	} else if (selmon == mons)
		for (m = mons; m->next; m = m->next);
	else
		for (m = mons; m->next != selmon; m = m->next);
	return m;
}

int
drawstatusbar(Monitor *m, int bh, char* stext) {
	int ret, i, j, w, x, len, stw;
	short isCode = 0;
	char *text;
	char *p;
	Clr oldbg, oldfg;

	len = strlen(stext) + 1 ;
	if (!(text = (char*) malloc(sizeof(char)*len)))
		die("malloc");
	p = text;

	i = -1, j = 0;
	while (stext[++i])
		if ((unsigned char)stext[i] >= ' ')
			text[j++] = stext[i];
	text[j] = '\0';
	/* compute width of the status text */
	w = 0;
	i = -1;
	while (text[++i]) {
		if (text[i] == '^') {
			if (!isCode) {
				isCode = 1;
				text[i] = '\0';
				w += TEXTW(text) - lrpad;
				text[i] = '^';
				if (text[++i] == 'f')
					w += atoi(text + ++i);
			} else {
				isCode = 0;
				text = text + i + 1;
				i = -1;
			}
		}
	}
	if (!isCode)
		w += TEXTW(text) - lrpad;
	else
		isCode = 0;
	text = p;

	w += 2; /* 1px padding on both sides */
	ret = m->ww - w;

	stw = (showsystray && m == systraytomon(m) && !systrayonleft) ?
		getsystraywidth() : 0;
	x = m->ww - w - stw;

	drw_setscheme(drw, scheme[LENGTH(colors)]);
	drw->scheme[ColFg] = scheme[SchemeNorm][ColFg];
	drw->scheme[ColBg] = scheme[SchemeNorm][ColBg];
	drw_rect(drw, x, 0, w, bh, 1, 1);
	x++;

	/* process status text */
	i = -1;
	while (text[++i]) {
		if (text[i] == '^' && !isCode) {
			isCode = 1;

			text[i] = '\0';
			w = TEXTW(text) - lrpad;
			drw_text(drw, x, 0, w, bh, 0, text, 0);

			x += w;

			/* process code */
			while (text[++i] != '^') {
				if (text[i] == 'c') {
					char buf[8];
					memcpy(buf, (char*)text+i+1, 7);
					buf[7] = '\0';
					drw_clr_create(drw, &drw->scheme[ColFg], buf);
					i += 7;
				} else if (text[i] == 'b') {
					char buf[8];
					memcpy(buf, (char*)text+i+1, 7);
					buf[7] = '\0';
					drw_clr_create(drw, &drw->scheme[ColBg], buf);
					i += 7;
				} else if (text[i] == 'S') {
					int clr = atoi(text + ++i);
					if (clr > LENGTH(colors)) clr = Urgent; // idiot check
					drw->scheme[ColFg] = scheme[clr][ColFg];
					drw->scheme[ColBg] = scheme[clr][ColBg];
				} else if (text[i] == 'F') {
					int clr = atoi(text + ++i);
					if (clr > LENGTH(colors)) clr = Urgent; // idiot check
					drw->scheme[ColFg] = scheme[clr][ColFg];
				} else if (text[i] == 'B') {
					int clr = atoi(text + ++i);
					if (clr > LENGTH(colors)) clr = Urgent; // idiot check
					drw->scheme[ColBg] = scheme[clr][ColBg];
				} else if (text[i] == 'd') {
					drw->scheme[ColFg] = scheme[SchemeNorm][ColFg];
					drw->scheme[ColBg] = scheme[SchemeNorm][ColBg];
				} else if (text[i] == 'w') {
					Clr swp;
					swp = drw->scheme[ColFg];
					drw->scheme[ColFg] = drw->scheme[ColBg];
					drw->scheme[ColBg] = swp;
				} else if (text[i] == 'v') {
					oldfg = drw->scheme[ColFg];
					oldbg = drw->scheme[ColBg];
				} else if (text[i] == 't') {
					drw->scheme[ColFg] = oldfg;
					drw->scheme[ColBg] = oldbg;
				} else if (text[i] == 'r') {
					int rx = atoi(text + ++i);
					while (text[++i] != ',');
					int ry = atoi(text + ++i);
					while (text[++i] != ',');
					int rw = atoi(text + ++i);
					while (text[++i] != ',');
					int rh = atoi(text + ++i);

					drw_rect(drw, rx + x, ry, rw, rh, 1, 0);
				} else if (text[i] == 'f') {
					x += atoi(text + ++i);
				} else if (text[i] == '\0') { /* please don't dump core if some script sends an unmatched ^ somewhere */
					drw_setscheme(drw, scheme[SchemeNorm]);
					free(p);
					return ret;
				}
			}

			text = text + i + 1;
			i=-1;
			isCode = 0;
		}
	}

	if (!isCode) {
		w = TEXTW(text) - lrpad;
		drw_text(drw, x, 0, w, bh, 0, text, 0);
	}

	drw_setscheme(drw, scheme[SchemeNorm]);
	free(p);

	return ret;
}

void
drawbar(Monitor *m)
{
	int x, w, tw = 0, stw = 0;
	int boxs = drw->fonts->h / 9;
	int boxw = drw->fonts->h / 6 + 2;
	unsigned int i, occ = 0, urg = 0, n = 0;
	Client *c;

	if (!m->showbar)
		return;

	if(showsystray && m == systraytomon(m) && !systrayonleft)
		stw = getsystraywidth();

	/* draw status first so it can be overdrawn by tags later */
	if (m == selmon || 1) { /* status is only drawn on selected monitor */
		tw = statusw = m->ww - drawstatusbar(m, bh, stext);
	}

	resizebarwin(m);
	for (c = m->clients; c; c = c->next) {
		if (ISVISIBLE(c))
			++n;
		occ |= c->tags;
		if (c->isurgent)
			urg |= c->tags;
	}
	x = 0;
	for (i = 0; i < LENGTH(tags); i++) {
		w = TEXTW(tags[i]);
		drw_setscheme(drw, scheme[m->tagset[m->seltags] & 1 << i ?  SchemeSel : SchemeNorm]);
		drw_text(drw, x, 0, w, bh, lrpad / 2, tags[i], urg & 1 << i);
		if (occ & 1 << i)
			drw_rect(drw, x + boxs, boxs, boxw, boxw,
				m == selmon && selmon->sel && selmon->sel->tags & 1 << i,
				urg & 1 << i);
		x += w;
	}
	w = blw = TEXTW(m->ltsymbol);
	drw_setscheme(drw, scheme[SchemeNorm]);
	x = drw_text(drw, x, 0, w, bh, lrpad / 2, m->ltsymbol, 0);

	if ((w = m->ww - tw - stw - x) > bh) {
		drw_setscheme(drw, scheme[SchemeNorm]);
		if (m->sel) {
			//drw_setscheme(drw, scheme[m == selmon ? SchemeSel : SchemeNorm]);
			drw_text(drw, x, 0, w, bh, lrpad / 2, m->sel->name, 0);
			if (m->sel->isfloating)
				drw_rect(drw, x + boxs, boxs, boxw, boxw, m->sel->isfixed, 0);
		} else {
			//drw_setscheme(drw, scheme[SchemeNorm]);
			drw_rect(drw, x, 0, w, bh, 1, 1);
		}
	}
	drw_map(drw, m->barwin, 0, 0, m->ww - stw, bh);
}

void
drawbars(void)
{
	Monitor *m;

	for (m = mons; m; m = m->next)
		drawbar(m);
}

void
enternotify(XEvent *e)
{
	Client *c;
	Monitor *m;
	XCrossingEvent *ev = &e->xcrossing;

	if ((ev->mode != NotifyNormal || ev->detail == NotifyInferior) && ev->window != root)
		return;
	c = wintoclient(ev->window);
	m = c ? c->mon : wintomon(ev->window);
	if (m != selmon) {
		unfocus(selmon->sel, 1);
		selmon = m;
	} else if (!c || c == selmon->sel)
		return;
	focus(c);
}

void
expose(XEvent *e)
{
	Monitor *m;
	XExposeEvent *ev = &e->xexpose;

	if (ev->count == 0 && (m = wintomon(ev->window))) {
		drawbar(m);
		if (m == selmon)
			updatesystray();
	}
}

void
focus(Client *c)
{
	if (!c || !ISVISIBLE(c))
		for (c = selmon->stack; c && !ISVISIBLE(c); c = c->snext);
	if (selmon->sel && selmon->sel != c)
		unfocus(selmon->sel, 0);
	if (c) {
		if (c->mon != selmon)
			selmon = c->mon;
		if (c->isurgent)
			seturgent(c, 0);
		detachstack(c);
		attachstack(c);
		grabbuttons(c, 1);
		XSetWindowBorder(dpy, c->win, scheme[SchemeSel][ColBorder].pixel);

                if (!selmon->pertag->drawwithgaps[selmon->pertag->curtag] && !c->isfloating) {
			XWindowChanges wc;
                        wc.sibling = selmon->barwin;
                        wc.stack_mode = Below;
                        XConfigureWindow(dpy, c->win, CWSibling | CWStackMode, &wc);
                }

		setfocus(c);
	} else {
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
	}
	if(selmon->sel && selmon->sel->isfullscreen){
		togglefullscreen(NULL);
		selmon->sel = c;
		togglefullscreen(NULL);
	} else {
		selmon->sel = c;
	}
	drawbars();
}

/* there are some broken focus acquiring clients needing extra handling */
void
focusin(XEvent *e)
{
	XFocusChangeEvent *ev = &e->xfocus;

	if (selmon->sel && ev->window != selmon->sel->win)
		setfocus(selmon->sel);
}

void
focusmaster(const Arg *arg)
{
	Client *master;

	if (selmon->nmaster > 1)
		return;
	if (!selmon->sel || (selmon->sel->isfullscreen && lockfullscreen))
		return;

	master = nexttiled(selmon->clients);

	if (!master)
		return;

	int i;
	for (i = 0; !(selmon->tagset[selmon->seltags] & 1 << i); i++);
	i++;

	if (selmon->sel == master) {
		if (selmon->tagmarked[i] && ISVISIBLE(selmon->tagmarked[i]))
			focus(selmon->tagmarked[i]);
	} else {
		selmon->tagmarked[i] = selmon->sel;
		focus(master);
	}
}


void
focusmon(const Arg *arg)
{
	Monitor *m;

	if (!mons->next)
		return;
	if ((m = dirtomon(arg->i)) == selmon)
		return;
	unfocus(selmon->sel, 0);
	selmon = m;
	focus(NULL);
	if (selmon->sel)
		XWarpPointer(dpy, None, selmon->sel->win, 0,0,0,0,
				selmon->sel->w/2, selmon->sel->h/2);
}

void
focusstack(const Arg *arg)
{
	Client *c = NULL, *i;

	if (!selmon->sel)
		return;
	if (arg->i > 0) {
		for (c = selmon->sel->next; c && !ISVISIBLE(c); c = c->next);
		if (!c)
			for (c = selmon->clients; c && !ISVISIBLE(c); c = c->next);
	} else {
		for (i = selmon->clients; i != selmon->sel; i = i->next)
			if (ISVISIBLE(i))
				c = i;
		if (!c)
			for (; i; i = i->next)
				if (ISVISIBLE(i))
					c = i;
	}
	if (c) {
		focus(c);
		restack(selmon);
	}
}

Atom
getatomprop(Client *c, Atom prop)
{
	int di;
	unsigned long dl;
	unsigned char *p = NULL;
	Atom da, atom = None;

	/* FIXME getatomprop should return the number of items and a pointer to
	 * the stored data instead of this workaround */
	Atom req = XA_ATOM;
	if (prop == xatom[XembedInfo])
		req = xatom[XembedInfo];

	if (XGetWindowProperty(dpy, c->win, prop, 0L, sizeof atom, False, req,
		&da, &di, &dl, &dl, &p) == Success && p) {
		atom = *(Atom *)p;
		if (da == xatom[XembedInfo] && dl == 2)
			atom = ((Atom *)p)[1];
		XFree(p);
	}
	return atom;
}

pid_t
getstatusbarpid()
{
	char buf[32], *str = buf, *c;
	FILE *fp;

	if (statuspid > 0) {
		snprintf(buf, sizeof(buf), "/proc/%u/cmdline", statuspid);
		if ((fp = fopen(buf, "r"))) {
			if (fgets(buf, sizeof(buf), fp)) {};
			while ((c = strchr(str, '/')))
				str = c + 1;
			fclose(fp);
			if (!strcmp(str, STATUSBAR))
				return statuspid;
		}
	}
	if (!(fp = popen("pidof -s "STATUSBAR, "r")))
		return -1;
	if (fgets(buf, sizeof(buf), fp)) {};
	pclose(fp);
	return strtoul(buf, NULL, 10);
}

unsigned int
getsystraywidth()
{
	unsigned int w = 0;
	Client *i;
	if(showsystray)
		for(i = systray->icons; i; w += i->w + systrayspacing, i = i->next) ;
	return w ? w + systrayspacing : 1;
}

int
getrootptr(int *x, int *y)
{
	int di;
	unsigned int dui;
	Window dummy;

	return XQueryPointer(dpy, root, &dummy, &dummy, x, y, &di, &di, &dui);
}

long
getstate(Window w)
{
	int format;
	long result = -1;
	unsigned char *p = NULL;
	unsigned long n, extra;
	Atom real;

	if (XGetWindowProperty(dpy, w, wmatom[WMState], 0L, 2L, False, wmatom[WMState],
		&real, &format, &n, &extra, (unsigned char **)&p) != Success)
		return -1;
	if (n != 0)
		result = *p;
	XFree(p);
	return result;
}

int
gettextprop(Window w, Atom atom, char *text, unsigned int size)
{
	char **list = NULL;
	int n;
	XTextProperty name;

	if (!text || size == 0)
		return 0;
	text[0] = '\0';
	if (!XGetTextProperty(dpy, w, &name, atom) || !name.nitems)
		return 0;
	if (name.encoding == XA_STRING)
		strncpy(text, (char *)name.value, size - 1);
	else {
		if (XmbTextPropertyToTextList(dpy, &name, &list, &n) >= Success && n > 0 && *list) {
			strncpy(text, *list, size - 1);
			XFreeStringList(list);
		}
	}
	text[size - 1] = '\0';
	XFree(name.value);
	return 1;
}

void
grabbuttons(Client *c, int focused)
{
	updatenumlockmask();
	{
		unsigned int i, j;
		unsigned int modifiers[] = { 0, LockMask, numlockmask, numlockmask|LockMask };
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		if (!focused)
			XGrabButton(dpy, AnyButton, AnyModifier, c->win, False,
				BUTTONMASK, GrabModeSync, GrabModeSync, None, None);
		for (i = 0; i < LENGTH(buttons); i++)
			if (buttons[i].click == ClkClientWin)
				for (j = 0; j < LENGTH(modifiers); j++)
					XGrabButton(dpy, buttons[i].button,
						buttons[i].mask | modifiers[j],
						c->win, False, BUTTONMASK,
						GrabModeAsync, GrabModeSync, None, None);
	}
}

void
grabkeys(void)
{
	updatenumlockmask();
	{
		unsigned int i, j;
		unsigned int modifiers[] = { 0, LockMask, numlockmask, numlockmask|LockMask };
		KeyCode code;

		XUngrabKey(dpy, AnyKey, AnyModifier, root);
		for (i = 0; i < LENGTH(keys); i++)
			if ((code = XKeysymToKeycode(dpy, keys[i].keysym)))
				for (j = 0; j < LENGTH(modifiers); j++)
					XGrabKey(dpy, code, keys[i].mod | modifiers[j], root,
						True, GrabModeAsync, GrabModeAsync);
	}
}

void
gridstack(Monitor *m)
{
	/* hack the tile and grid layouts together */
	unsigned int cols, rows, cn, rn, rh, cw, cx, cy, nstack;
	unsigned int i, n, h, mw, my;
	unsigned int gap;
	Client *c;

	for (n = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), n++);
	if (n == 0)
		return;

	gap = m->pertag->drawwithgaps[m->pertag->curtag] ? m->pertag->gappx[m->pertag->curtag] : 0;
	if (n > m->nmaster)
		mw = m->nmaster ? m->ww * m->mfact : 0;
	else
		mw = m->ww - gap;

	nstack = n - m->nmaster;

	/* grid dimensions */
	if (nstack == 3 || nstack == 5) rows = 2; /* override grid calc for these cases */
	else
		for(rows = 0; rows <= nstack/2; rows++)
			if(rows*rows >= nstack) break;
	cols = rows ? nstack/rows : 0;

	/* window geometries */
	rh = rows ? (m->wh - gap)/rows : m->wh - gap;
	cn = 0; /* current column number */
	rn = 0; /* current row number */

	if (m->pertag->drawwithgaps[m->pertag->curtag]) {
		for (i = 0, my = gap, c = nexttiled(m->clients); c; c = nexttiled(c->next), i++)
			if (i < m->nmaster) {
				h = (m->wh - my) / (MIN(n, m->nmaster) - i) - gap;
				resize(c, m->wx + gap, m->wy + my, mw - 2*c->bw - gap, h - 2*c->bw, False);
				if (my + HEIGHT(c) + gap < m->wh)
					my += HEIGHT(c) + gap;
			} else {
				if((i - m->nmaster)/cols + 1 > rows - nstack%rows)
					cols = nstack/rows + 1;
				cw = cols ? (m->ww - mw - gap) / cols : m->ww - mw - gap;
				cx = m->wx + mw + cn*cw;
				cy = m->wy + rn*rh;
				resize(c, cx + gap, cy + gap, cw - 2 * c->bw - gap, rh - 2 * c->bw - gap, False);
				++cn;
				if(cn >= cols) {
					cn = 0;
					rn++;
				}
			}
	} else {
		for (i = my = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), i++)
			if (i < m->nmaster) {
				h = (m->wh - my) / (MIN(n, m->nmaster) - i);
				if (n == 1)
					resize(c, m->wx - c->bw, m->wy, m->ww - c->bw, m->wh - c->bw, False);
				else
					resize(c, m->wx - c->bw, m->wy + my, mw - c->bw, h - c->bw, False);
				my += HEIGHT(c) - c->bw;
			} else {
				if((i - m->nmaster)/cols + 1 > rows - nstack%rows)
					cols = nstack/rows + 1;
				cw = cols ? (m->ww - mw) / cols : m->ww - mw;
				cx = m->wx + mw + cn*cw;
				cy = m->wy + rn*rh;
				resize(c, cx, cy, cw - c->bw, rh - c->bw, False);
				++cn;
				if(cn >= cols) {
					cn = 0;
					rn++;
				}
			}
	}
}

int
handlexevent(struct epoll_event *ev)
{
	if (ev->events & EPOLLIN) {
		XEvent ev;
		while (running && XPending(dpy)) {
			XNextEvent(dpy, &ev);
			if (handler[ev.type]) {
				handler[ev.type](&ev); /* call handler */
				ipc_send_events(mons, &lastselmon, selmon);
			}
		}
	} else if (ev-> events & EPOLLHUP) {
		return -1;
	}

	return 0;
}

void
incnmaster(const Arg *arg)
{
	selmon->nmaster = selmon->pertag->nmasters[selmon->pertag->curtag] = MAX(selmon->nmaster + arg->i, 0);
	arrange(selmon);
}

#ifdef XINERAMA
static int
isuniquegeom(XineramaScreenInfo *unique, size_t n, XineramaScreenInfo *info)
{
	while (n--)
		if (unique[n].x_org == info->x_org && unique[n].y_org == info->y_org
		&& unique[n].width == info->width && unique[n].height == info->height)
			return 0;
	return 1;
}
#endif /* XINERAMA */

void
keypress(XEvent *e)
{
	unsigned int i;
	KeySym keysym;
	XKeyEvent *ev;

	ev = &e->xkey;
	keysym = XKeycodeToKeysym(dpy, (KeyCode)ev->keycode, 0);
	for (i = 0; i < LENGTH(keys); i++)
		if (keysym == keys[i].keysym
		&& CLEANMASK(keys[i].mod) == CLEANMASK(ev->state)
		&& keys[i].func)
			keys[i].func(&(keys[i].arg));
}

void
killclient(const Arg *arg)
{
	if (!selmon->sel)
		return;

	if (!sendevent(selmon->sel->win, wmatom[WMDelete], NoEventMask, wmatom[WMDelete], CurrentTime, 0 , 0, 0)) {
		XGrabServer(dpy);
		XSetErrorHandler(xerrordummy);
		XSetCloseDownMode(dpy, DestroyAll);
		XKillClient(dpy, selmon->sel->win);
		XSync(dpy, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dpy);
	}
}

void
loadxrdb()
{
	Display *display;
	char * resm;
	XrmDatabase xrdb;
	char *type;
	XrmValue value;

	display = XOpenDisplay(NULL);

	if (display != NULL) {
		resm = XResourceManagerString(display);

		if (resm != NULL) {
			xrdb = XrmGetStringDatabase(resm);

			if (xrdb != NULL) {
				XRDB_LOAD_COLOR("dwm.normbordercolor", normbordercolor);
				XRDB_LOAD_COLOR("dwm.normbgcolor", normbgcolor);
				XRDB_LOAD_COLOR("dwm.normfgcolor", normfgcolor);
				XRDB_LOAD_COLOR("dwm.selbordercolor", selbordercolor);
				XRDB_LOAD_COLOR("dwm.selbgcolor", selbgcolor);
				XRDB_LOAD_COLOR("dwm.selfgcolor", selfgcolor);

				XrmGetResource(xrdb, "dwm.borderpx", "*", &type, &value);
				if (value.addr != NULL && !strncmp("String", type, 64))
					borderpx = strtoul(value.addr, NULL, 10);
				else
					borderpx = 1;
			}
		}
	}

	XCloseDisplay(display);
}

void
manage(Window w, XWindowAttributes *wa)
{
	Client *c, *t = NULL, *term = NULL;
	Window trans = None;
	XWindowChanges wc;

	c = ecalloc(1, sizeof(Client));
	c->win = w;
	c->pid = winpid(w);
	/* geometry */
	c->x = c->oldx = wa->x;
	c->y = c->oldy = wa->y;
	c->w = c->oldw = wa->width;
	c->h = c->oldh = wa->height;
	c->oldbw = wa->border_width;

	updatetitle(c);
	if (XGetTransientForHint(dpy, w, &trans) && (t = wintoclient(trans))) {
		c->mon = t->mon;
		c->tags = t->tags;
	} else {
		c->mon = selmon;
		applyrules(c);
		term = termforwin(c);
	}

	if (c->x + WIDTH(c) > c->mon->mx + c->mon->mw)
		c->x = c->mon->mx + c->mon->mw - WIDTH(c);
	if (c->y + HEIGHT(c) > c->mon->my + c->mon->mh)
		c->y = c->mon->my + c->mon->mh - HEIGHT(c);
	c->x = MAX(c->x, c->mon->mx);
	/* only fix client y-offset, if the client center might cover the bar */
	c->y = MAX(c->y, ((c->mon->by == c->mon->my) && (c->x + (c->w / 2) >= c->mon->wx)
		&& (c->x + (c->w / 2) < c->mon->wx + c->mon->ww)) ? bh : c->mon->my);
	c->bw = c->mon->borderpx;

	selmon->tagset[selmon->seltags] &= ~scratchtag;
	if (!strcmp(c->name, scratchpadname)) {
		c->mon->tagset[c->mon->seltags] |= c->tags = scratchtag;
		c->isfloating = True;
		c->x = c->mon->wx + (c->mon->ww / 2 - WIDTH(c) / 2);
		c->y = c->mon->wy + (c->mon->wh / 2 - HEIGHT(c) / 2);
	}

	wc.border_width = c->bw;
	XConfigureWindow(dpy, w, CWBorderWidth, &wc);
	XSetWindowBorder(dpy, w, scheme[SchemeNorm][ColBorder].pixel);
	configure(c); /* propagates border_width, if size doesn't change */
	updatewindowtype(c);
	updatesizehints(c);
	updatewmhints(c);
	updatemotifhints(c);
	{
		int format;
		unsigned long *data, n, extra;
		Monitor *m;
		Atom atom;
		if (XGetWindowProperty(dpy, c->win, netatom[NetClientInfo], 0L, 2L, False, XA_CARDINAL,
				&atom, &format, &n, &extra, (unsigned char **)&data) == Success && n == 2) {
			c->tags = *data;
			for (m = mons; m; m = m->next) {
				if (m->num == *(data+1)) {
					c->mon = m;
					break;
				}
			}
		}
		if (n > 0)
			XFree(data);
	}
	setclienttagprop(c);

	XSelectInput(dpy, w, EnterWindowMask|FocusChangeMask|PropertyChangeMask|StructureNotifyMask);
	grabbuttons(c, 0);
	if (!c->isfloating)
		c->isfloating = c->oldstate = trans != None || c->isfixed;
	if (c->isfloating)
		XRaiseWindow(dpy, c->win);
	attachaside(c);
	attachstack(c);
	XChangeProperty(dpy, root, netatom[NetClientList], XA_WINDOW, 32, PropModeAppend,
		(unsigned char *) &(c->win), 1);
	XMoveResizeWindow(dpy, c->win, c->x + 2 * sw, c->y, c->w, c->h); /* some windows require this */
	setclientstate(c, NormalState);
	if (c->mon == selmon)
		unfocus(selmon->sel, 0);
	c->mon->sel = c;
	arrange(c->mon);
	XMapWindow(dpy, c->win);
	if (term)
		swallow(term, c);
	focus(NULL);
}

void
mappingnotify(XEvent *e)
{
	XMappingEvent *ev = &e->xmapping;

	XRefreshKeyboardMapping(ev);
	if (ev->request == MappingKeyboard)
		grabkeys();
}

void
maprequest(XEvent *e)
{
	static XWindowAttributes wa;
	XMapRequestEvent *ev = &e->xmaprequest;

	Client *i;
	if ((i = wintosystrayicon(ev->window))) {
		sendevent(i->win, netatom[Xembed], StructureNotifyMask, CurrentTime, XEMBED_WINDOW_ACTIVATE, 0, systray->win, XEMBED_EMBEDDED_VERSION);
		resizebarwin(selmon);
		updatesystray();
	}

	if (!XGetWindowAttributes(dpy, ev->window, &wa) || wa.override_redirect)
		return;
	if (!wintoclient(ev->window))
		manage(ev->window, &wa);
}

void
monocle(Monitor *m)
{
	unsigned int n = 0;
	Client *c;

	for (c = m->clients; c; c = c->next)
		if (ISVISIBLE(c))
			n++;
	if (n > 0) /* override layout symbol */
		snprintf(m->ltsymbol, sizeof m->ltsymbol, "[%d]", n);
	for (c = nexttiled(m->clients); c; c = nexttiled(c->next))
		if (selmon->pertag->drawwithgaps[selmon->pertag->curtag])
			resize(c, m->wx + selmon->pertag->gappx[m->pertag->curtag],
			          m->wy + selmon->pertag->gappx[m->pertag->curtag],
			          m->ww - 2 * (c->bw + selmon->pertag->gappx[m->pertag->curtag]),
			          m->wh - 2 * (c->bw + selmon->pertag->gappx[m->pertag->curtag]),
			          0);
		else
			resize(c, m->wx - c->bw, m->wy, m->ww, m->wh - c->bw, False);
}

void
motionnotify(XEvent *e)
{
	static Monitor *mon = NULL;
	Monitor *m;
	XMotionEvent *ev = &e->xmotion;

	if (ev->window != root)
		return;
	if ((m = recttomon(ev->x_root, ev->y_root, 1, 1)) != mon && mon) {
		unfocus(selmon->sel, 1);
		selmon = m;
		focus(NULL);
	}
	mon = m;
}

void
movemouse(const Arg *arg)
{
	int x, y, ocx, ocy, nx, ny;
	Client *c;
	Monitor *m;
	XEvent ev;
	Time lasttime = 0;

	if (!(c = selmon->sel))
		return;
	if (c->isfullscreen) /* no support moving fullscreen windows by mouse */
		return;
	restack(selmon);
	ocx = c->x;
	ocy = c->y;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
		None, cursor[CurMove]->cursor, CurrentTime) != GrabSuccess)
		return;
	if (!getrootptr(&x, &y))
		return;
	do {
		XMaskEvent(dpy, MOUSEMASK|ExposureMask|SubstructureRedirectMask, &ev);
		switch(ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / 60))
				continue;
			lasttime = ev.xmotion.time;

			nx = ocx + (ev.xmotion.x - x);
			ny = ocy + (ev.xmotion.y - y);
			if (abs(selmon->wx - nx) < snap)
				nx = selmon->wx;
			else if (abs((selmon->wx + selmon->ww) - (nx + WIDTH(c))) < snap)
				nx = selmon->wx + selmon->ww - WIDTH(c);
			if (abs(selmon->wy - ny) < snap)
				ny = selmon->wy;
			else if (abs((selmon->wy + selmon->wh) - (ny + HEIGHT(c))) < snap)
				ny = selmon->wy + selmon->wh - HEIGHT(c);
			if (!c->isfloating && selmon->lt[selmon->sellt]->arrange
			&& (abs(nx - c->x) > snap || abs(ny - c->y) > snap))
				togglefloating(NULL);
			if (!selmon->lt[selmon->sellt]->arrange || c->isfloating)
				resize(c, nx, ny, c->w, c->h, 1);
			break;
		}
	} while (ev.type != ButtonRelease);
	XUngrabPointer(dpy, CurrentTime);
	if ((m = recttomon(c->x, c->y, c->w, c->h)) != selmon) {
		sendmon(c, m);
		selmon = m;
		focus(NULL);
	}
}

Client *
nexttagged(Client *c) {
	Client *walked = c->mon->clients;
	for(;
		walked && (walked->isfloating || !ISVISIBLEONTAG(walked, c->tags));
		walked = walked->next
	);
	return walked;
}

Client *
nexttiled(Client *c)
{
	for (; c && (c->isfloating || !ISVISIBLE(c)); c = c->next);
	return c;
}

void
pop(Client *c)
{
	int i;
	for (i = 0; !(selmon->tagset[selmon->seltags] & 1 << i); i++);
	i++;

	c->mon->tagmarked[i] = nexttiled(c->mon->clients);
	detach(c);
	attach(c);
	focus(c);
	arrange(c->mon);
}

Client *
prevtiled(Client *c) {
	Client *p, *r;

	for(p = selmon->clients, r = NULL; p && p != c; p = p->next)
		if(!p->isfloating && ISVISIBLE(p))
			r = p;
	return r;
}

void
propertynotify(XEvent *e)
{
	Client *c;
	Window trans;
	XPropertyEvent *ev = &e->xproperty;

	if ((c = wintosystrayicon(ev->window))) {
		if (ev->atom == XA_WM_NORMAL_HINTS) {
			updatesizehints(c);
			updatesystrayicongeom(c, c->w, c->h);
		}
		else
			updatesystrayiconstate(c, ev);
		resizebarwin(selmon);
		updatesystray();
	}

	if ((ev->window == root) && (ev->atom == XA_WM_NAME))
		updatestatus();
	else if (ev->state == PropertyDelete)
		return; /* ignore */
	else if ((c = wintoclient(ev->window))) {
		switch(ev->atom) {
		default: break;
		case XA_WM_TRANSIENT_FOR:
			if (!c->isfloating && (XGetTransientForHint(dpy, c->win, &trans)) &&
				(c->isfloating = (wintoclient(trans)) != NULL))
				arrange(c->mon);
			break;
		case XA_WM_NORMAL_HINTS:
			updatesizehints(c);
			break;
		case XA_WM_HINTS:
			updatewmhints(c);
			drawbars();
			break;
		}
		if (ev->atom == XA_WM_NAME || ev->atom == netatom[NetWMName]) {
			updatetitle(c);
			if (c == c->mon->sel)
				drawbar(c->mon);
		}
		if (ev->atom == netatom[NetWMWindowType])
			updatewindowtype(c);
		if (ev->atom == motifatom)
			updatemotifhints(c);
	}
}

void
pushdown(const Arg *arg) {
	Client *sel = selmon->sel, *c;

	if(!sel || sel->isfloating || sel == nexttiled(selmon->clients))
		return;
	if((c = nexttiled(sel->next))) {
		detach(sel);
		sel->next = c->next;
		c->next = sel;
	}
	focus(sel);
	arrange(selmon);
}

void
pushup(const Arg *arg) {
	Client *sel = selmon->sel, *c;

	if(!sel || sel->isfloating)
		return;
	if((c = prevtiled(sel)) && c != nexttiled(selmon->clients)) {
		detach(sel);
		sel->next = c;
		for(c = selmon->clients; c->next != sel->next; c = c->next);
		c->next = sel;
	}
	focus(sel);
	arrange(selmon);
}

void
quit(const Arg *arg)
{
	size_t i;

	/* kill child processes */
	for (i = 0; i < autostart_len; i++) {
		if (0 < autostart_pids[i]) {
			kill(autostart_pids[i], SIGTERM);
			waitpid(autostart_pids[i], NULL, 0);
		}
	}

	if(arg->i) restart = 1;
	running = 0;
}

Monitor *
recttomon(int x, int y, int w, int h)
{
	Monitor *m, *r = selmon;
	int a, area = 0;

	for (m = mons; m; m = m->next)
		if ((a = INTERSECT(x, y, w, h, m)) > area) {
			area = a;
			r = m;
		}
	return r;
}

void
removesystrayicon(Client *i)
{
	Client **ii;

	if (!showsystray || !i)
		return;
	for (ii = &systray->icons; *ii && *ii != i; ii = &(*ii)->next);
	if (ii)
		*ii = i->next;
	free(i);
}

void
resize(Client *c, int x, int y, int w, int h, int interact)
{
	if (applysizehints(c, &x, &y, &w, &h, interact))
		resizeclient(c, x, y, w, h);
}

void
resizebarwin(Monitor *m) {
	unsigned int w = m->ww;
	if (showsystray && m == systraytomon(m) && !systrayonleft)
		w -= getsystraywidth();
	XMoveResizeWindow(dpy, m->barwin, m->wx, m->by, w, bh);
}

void
resizeclient(Client *c, int x, int y, int w, int h)
{
	XWindowChanges wc;

	c->oldx = c->x; c->x = wc.x = x;
	c->oldy = c->y; c->y = wc.y = y;
	c->oldw = c->w; c->w = wc.width = w;
	c->oldh = c->h; c->h = wc.height = h;
	wc.border_width = c->bw;

	if (!selmon->pertag->drawwithgaps[selmon->pertag->curtag] && /* this is the noborderfloatingfix patch, slightly modified so that it will work if, and only if, gaps are disabled. */
	    (((nexttiled(c->mon->clients) == c && !nexttiled(c->next)) /* these two first lines are the only ones changed. if you are manually patching and have noborder installed already, just change these lines; or conversely, just remove this section if the noborder patch is not desired;) */
	    || &monocle == c->mon->lt[c->mon->sellt]->arrange))
	    && !c->isfullscreen && !c->isfloating
	    && NULL != c->mon->lt[c->mon->sellt]->arrange) {
	        c->w = wc.width += c->bw * 2;
	        c->h = wc.height += c->bw * 2;
	        wc.border_width = 0;
	}
	XConfigureWindow(dpy, c->win, CWX|CWY|CWWidth|CWHeight|CWBorderWidth, &wc);
	configure(c);
	XSync(dpy, False);
}

void
resizerequest(XEvent *e)
{
	XResizeRequestEvent *ev = &e->xresizerequest;
	Client *i;

	if ((i = wintosystrayicon(ev->window))) {
		updatesystrayicongeom(i, ev->width, ev->height);
		resizebarwin(selmon);
		updatesystray();
	}
}

void
resizemouse(const Arg *arg)
{
	int ocx, ocy, nw, nh;
	Client *c;
	Monitor *m;
	XEvent ev;
	Time lasttime = 0;

	if (!(c = selmon->sel))
		return;
	if (c->isfullscreen) /* no support resizing fullscreen windows by mouse */
		return;
	restack(selmon);
	ocx = c->x;
	ocy = c->y;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
		None, cursor[CurResize]->cursor, CurrentTime) != GrabSuccess)
		return;
	XWarpPointer(dpy, None, c->win, 0, 0, 0, 0, c->w + c->bw - 1, c->h + c->bw - 1);
	do {
		XMaskEvent(dpy, MOUSEMASK|ExposureMask|SubstructureRedirectMask, &ev);
		switch(ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / 60))
				continue;
			lasttime = ev.xmotion.time;

			nw = MAX(ev.xmotion.x - ocx - 2 * c->bw + 1, 1);
			nh = MAX(ev.xmotion.y - ocy - 2 * c->bw + 1, 1);
			if (c->mon->wx + nw >= selmon->wx && c->mon->wx + nw <= selmon->wx + selmon->ww
			&& c->mon->wy + nh >= selmon->wy && c->mon->wy + nh <= selmon->wy + selmon->wh)
			{
				if (!c->isfloating && selmon->lt[selmon->sellt]->arrange
				&& (abs(nw - c->w) > snap || abs(nh - c->h) > snap))
					togglefloating(NULL);
			}
			if (!selmon->lt[selmon->sellt]->arrange || c->isfloating)
				resize(c, c->x, c->y, nw, nh, 1);
			break;
		}
	} while (ev.type != ButtonRelease);
	XWarpPointer(dpy, None, c->win, 0, 0, 0, 0, c->w + c->bw - 1, c->h + c->bw - 1);
	XUngrabPointer(dpy, CurrentTime);
	while (XCheckMaskEvent(dpy, EnterWindowMask, &ev));
	if ((m = recttomon(c->x, c->y, c->w, c->h)) != selmon) {
		sendmon(c, m);
		selmon = m;
		focus(NULL);
	}
}

void
restack(Monitor *m)
{
	Client *c;
	XEvent ev;
	XWindowChanges wc;

	drawbar(m);
	if (!m->sel)
		return;
	if (m->sel->isfloating || !m->lt[m->sellt]->arrange)
		XRaiseWindow(dpy, m->sel->win);
	if (m->lt[m->sellt]->arrange) {
		wc.stack_mode = Below;
		wc.sibling = m->barwin;
		for (c = m->stack; c; c = c->snext)
			if (!c->isfloating && ISVISIBLE(c)) {
				XConfigureWindow(dpy, c->win, CWSibling|CWStackMode, &wc);
				wc.sibling = c->win;
			}
	}
	XSync(dpy, False);
	while (XCheckMaskEvent(dpy, EnterWindowMask, &ev));
}

void
run(void)
{
	int event_count = 0;
	const int MAX_EVENTS = 10;
	struct epoll_event events[MAX_EVENTS];

	XSync(dpy, False);

	/* main event loop */
	while (running) {
		event_count = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

		for (int i = 0; i < event_count; i++) {
			int event_fd = events[i].data.fd;
			DEBUG("Got event from fd %d\n", event_fd);

			if (event_fd == dpy_fd) {
				// -1 means EPOLLHUP
				if (handlexevent(events + i) == -1)
					return;
			} else if (event_fd == ipc_get_sock_fd()) {
				ipc_handle_socket_epoll_event(events + i);
			} else if (ipc_is_client_registered(event_fd)){
				if (ipc_handle_client_epoll_event(events + i, mons, &lastselmon, selmon,
							tags, LENGTH(tags), layouts, LENGTH(layouts)) < 0) {
					fprintf(stderr, "Error handling IPC event on fd %d\n", event_fd);
				}
			} else {
				fprintf(stderr, "Got event from unknown fd %d, ptr %p, u32 %d, u64 %lu",
						event_fd, events[i].data.ptr, events[i].data.u32,
						events[i].data.u64);
				fprintf(stderr, " with events %d\n", events[i].events);
				return;
			}
		}
	}
}

void
scan(void)
{
	unsigned int i, num;
	Window d1, d2, *wins = NULL;
	XWindowAttributes wa;

	if (XQueryTree(dpy, root, &d1, &d2, &wins, &num)) {
		for (i = 0; i < num; i++) {
			if (!XGetWindowAttributes(dpy, wins[i], &wa)
			|| wa.override_redirect || XGetTransientForHint(dpy, wins[i], &d1))
				continue;
			if (wa.map_state == IsViewable || getstate(wins[i]) == IconicState)
				manage(wins[i], &wa);
		}
		for (i = 0; i < num; i++) { /* now the transients */
			if (!XGetWindowAttributes(dpy, wins[i], &wa))
				continue;
			if (XGetTransientForHint(dpy, wins[i], &d1)
			&& (wa.map_state == IsViewable || getstate(wins[i]) == IconicState))
				manage(wins[i], &wa);
		}
		if (wins)
			XFree(wins);
	}
}

void
sendmon(Client *c, Monitor *m)
{
	if (c->mon == m)
		return;
	unfocus(c, 1);
	detach(c);
	detachstack(c);
	c->mon = m;
	c->tags = m->tagset[m->seltags]; /* assign tags of target monitor */
	attachaside(c);
	attachstack(c);
	setclienttagprop(c);
	focus(NULL);
	arrange(NULL);
}

void
setborderpx(void)
{
	Monitor *m;
	Client *c;

	for (m = mons; m; m = m->next) {
		m->borderpx = borderpx;
		for (c = m->clients; c; c = c->next)
		{	
			c->bw = m->borderpx;
			if (c->isfloating || !m->lt[m->sellt]->arrange)
				resize(c, c->x, c->y, c->w, c->h, 0);
		}
	}
	arrange(NULL);
}

void
setclientstate(Client *c, long state)
{
	long data[] = { state, None };

	XChangeProperty(dpy, c->win, wmatom[WMState], wmatom[WMState], 32,
		PropModeReplace, (unsigned char *)data, 2);
}

int
sendevent(Window w, Atom proto, int mask, long d0, long d1, long d2, long d3, long d4)
{
	int n;
	Atom *protocols, mt;
	int exists = 0;
	XEvent ev;

	if (proto == wmatom[WMTakeFocus] || proto == wmatom[WMDelete]) {
		mt = wmatom[WMProtocols];
		if (XGetWMProtocols(dpy, w, &protocols, &n)) {
			while (!exists && n--)
				exists = protocols[n] == proto;
			XFree(protocols);
		}
	}
	else {
		exists = True;
		mt = proto;
	}

	if (exists) {
		ev.type = ClientMessage;
		ev.xclient.window = w;
		ev.xclient.message_type = mt;
		ev.xclient.format = 32;
		ev.xclient.data.l[0] = d0;
		ev.xclient.data.l[1] = d1;
		ev.xclient.data.l[2] = d2;
		ev.xclient.data.l[3] = d3;
		ev.xclient.data.l[4] = d4;
		XSendEvent(dpy, w, False, mask, &ev);
	}
	return exists;
}

void
setfocus(Client *c)
{
	if (!c->neverfocus) {
		XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
		XChangeProperty(dpy, root, netatom[NetActiveWindow],
			XA_WINDOW, 32, PropModeReplace,
			(unsigned char *) &(c->win), 1);
	}
	sendevent(c->win, wmatom[WMTakeFocus], NoEventMask, wmatom[WMTakeFocus], CurrentTime, 0, 0, 0);
}

void
setfullscreen(Client *c, int fullscreen)
{
	if (fullscreen && !c->isfullscreen) {
		XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
			PropModeReplace, (unsigned char*)&netatom[NetWMFullscreen], 1);
		c->isfullscreen = 1;
		c->oldstate = c->isfloating;
		c->oldbw = c->bw;
		c->bw = 0;
		c->isfloating = 1;
		resizeclient(c, c->mon->mx, c->mon->my, c->mon->mw, c->mon->mh);
		XRaiseWindow(dpy, c->win);
	} else if (!fullscreen && c->isfullscreen){
		XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
			PropModeReplace, (unsigned char*)0, 0);
		c->isfullscreen = 0;
		c->isfloating = c->oldstate;
		c->bw = c->oldbw;
		c->x = c->oldx;
		c->y = c->oldy;
		c->w = c->oldw;
		c->h = c->oldh;
		resizeclient(c, c->x, c->y, c->w, c->h);
		arrange(c->mon);
	}
}

void
setgaps(const Arg *arg)
{
	switch(arg->i)
	{
		case GAP_TOGGLE:
			selmon->pertag->drawwithgaps[selmon->pertag->curtag] = !selmon->pertag->drawwithgaps[selmon->pertag->curtag];
			break;
		case GAP_RESET:
			if (selmon->pertag->curtag > 0)
				selmon->pertag->gappx[selmon->pertag->curtag] = gappx[selmon->pertag->curtag - 1 % LENGTH(gappx)];
			else
				selmon->pertag->gappx[0] = gappx[0];
			break;
		default:
			if (selmon->pertag->gappx[selmon->pertag->curtag] + arg->i < 0)
				selmon->pertag->gappx[selmon->pertag->curtag] = 0;
			else
				selmon->pertag->gappx[selmon->pertag->curtag] += arg->i;
	}
	arrange(selmon);
}

void
setgappx(const Arg *arg)
{
	selmon->pertag->gappx[selmon->pertag->curtag] = arg->i;
	arrange(selmon);
}

void
setlayout(const Arg *arg)
{
	if (!arg || !arg->v || arg->v != selmon->lt[selmon->sellt])
		selmon->sellt = selmon->pertag->sellts[selmon->pertag->curtag] ^= 1;
	if (arg && arg->v)
		selmon->lt[selmon->sellt] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt] = (Layout *)arg->v;
	strncpy(selmon->ltsymbol, selmon->lt[selmon->sellt]->symbol, sizeof selmon->ltsymbol);
	if (selmon->sel)
		arrange(selmon);
	else
		drawbar(selmon);
}

__attribute__((unused))
void
setlayoutsafe(const Arg *arg)
{
	const Layout *ltptr = (Layout *)arg->v;
	if (ltptr == 0)
			setlayout(arg);
	for (int i = 0; i < LENGTH(layouts); i++) {
		if (ltptr == &layouts[i])
			setlayout(arg);
	}
}

/* arg > 1.0 will set mfact absolutely */
void
setmfact(const Arg *arg)
{
	float f;

	if (!arg || !selmon->lt[selmon->sellt]->arrange)
		return;
	f = arg->f < 1.0 ? arg->f + selmon->mfact : arg->f - 1.0;
	if (f < 0.05 || f > 0.95)
		return;
	selmon->mfact = selmon->pertag->mfacts[selmon->pertag->curtag] = f;
	arrange(selmon);
}

void
setup(void)
{
	int i;
	XSetWindowAttributes wa;
	Atom utf8string;

	/* clean up any zombies immediately */
	sigchld(0);

	signal(SIGHUP, sighup);
	signal(SIGTERM, sigterm);

	/* init screen */
	screen = DefaultScreen(dpy);
	sw = DisplayWidth(dpy, screen);
	sh = DisplayHeight(dpy, screen);
	root = RootWindow(dpy, screen);
	drw = drw_create(dpy, screen, root, sw, sh);
	if (!drw_fontset_create(drw, fonts, LENGTH(fonts)))
		die("no fonts could be loaded.");
	lrpad = drw->fonts->h;
	bh = drw->fonts->h + 2;
	updategeom();
	/* init atoms */
	utf8string = XInternAtom(dpy, "UTF8_STRING", False);
	wmatom[WMProtocols] = XInternAtom(dpy, "WM_PROTOCOLS", False);
	wmatom[WMDelete] = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	wmatom[WMState] = XInternAtom(dpy, "WM_STATE", False);
	wmatom[WMTakeFocus] = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
	netatom[NetActiveWindow] = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	netatom[NetSupported] = XInternAtom(dpy, "_NET_SUPPORTED", False);
	netatom[NetSystemTray] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_S0", False);
	netatom[NetSystemTrayOP] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
	netatom[NetSystemTrayOrientation] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
	netatom[NetSystemTrayOrientationHorz] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION_HORZ", False);
	netatom[NetWMName] = XInternAtom(dpy, "_NET_WM_NAME", False);
	netatom[NetWMState] = XInternAtom(dpy, "_NET_WM_STATE", False);
	netatom[NetWMCheck] = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
	netatom[NetWMFullscreen] = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	netatom[NetWMWindowType] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	netatom[NetWMWindowTypeDialog] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	netatom[NetClientList] = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
	motifatom = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
	netatom[NetClientInfo] = XInternAtom(dpy, "_NET_CLIENT_INFO", False);
	xatom[Manager] = XInternAtom(dpy, "MANAGER", False);
	xatom[Xembed] = XInternAtom(dpy, "_XEMBED", False);
	xatom[XembedInfo] = XInternAtom(dpy, "_XEMBED_INFO", False);
	/* init cursors */
	cursor[CurNormal] = drw_cur_create(drw, XC_left_ptr);
	cursor[CurResize] = drw_cur_create(drw, XC_sizing);
	cursor[CurMove] = drw_cur_create(drw, XC_fleur);
	/* init appearance */
	scheme = ecalloc(LENGTH(colors) + 1, sizeof(Clr *));
	scheme[LENGTH(colors)] = drw_scm_create(drw, colors[0], 3);
	for (i = 0; i < LENGTH(colors); i++)
		scheme[i] = drw_scm_create(drw, colors[i], 3);
	/* init system tray */
	updatesystray();
	/* init bars */
	updatebars();
	updatestatus();
	/* supporting window for NetWMCheck */
	wmcheckwin = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
	XChangeProperty(dpy, wmcheckwin, netatom[NetWMCheck], XA_WINDOW, 32,
		PropModeReplace, (unsigned char *) &wmcheckwin, 1);
	XChangeProperty(dpy, wmcheckwin, netatom[NetWMName], utf8string, 8,
		PropModeReplace, (unsigned char *) "dwm", 3);
	XChangeProperty(dpy, root, netatom[NetWMCheck], XA_WINDOW, 32,
		PropModeReplace, (unsigned char *) &wmcheckwin, 1);
	/* EWMH support per view */
	XChangeProperty(dpy, root, netatom[NetSupported], XA_ATOM, 32,
		PropModeReplace, (unsigned char *) netatom, NetLast);
	XDeleteProperty(dpy, root, netatom[NetClientList]);
	XDeleteProperty(dpy, root, netatom[NetClientInfo]);
	/* select events */
	wa.cursor = cursor[CurNormal]->cursor;
	wa.event_mask = SubstructureRedirectMask|SubstructureNotifyMask
		|ButtonPressMask|PointerMotionMask|EnterWindowMask
		|LeaveWindowMask|StructureNotifyMask|PropertyChangeMask;
	XChangeWindowAttributes(dpy, root, CWEventMask|CWCursor, &wa);
	XSelectInput(dpy, root, wa.event_mask);
	grabkeys();
	focus(NULL);
	setupepoll();
}

void
setupepoll(void)
{
	epoll_fd = epoll_create1(0);
	dpy_fd = ConnectionNumber(dpy);
	struct epoll_event dpy_event;

	// Initialize struct to 0
	memset(&dpy_event, 0, sizeof(dpy_event));

	DEBUG("Display socket is fd %d\n", dpy_fd);

	if (epoll_fd == -1) {
		fputs("Failed to create epoll file descriptor", stderr);
	}

	dpy_event.events = EPOLLIN;
	dpy_event.data.fd = dpy_fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, dpy_fd, &dpy_event)) {
		fputs("Failed to add display file descriptor to epoll", stderr);
		close(epoll_fd);
		exit(1);
	}

	if (ipc_init(ipcsockpath, epoll_fd, ipccommands, LENGTH(ipccommands)) < 0) {
		fputs("Failed to initialize IPC\n", stderr);
	}
}

void
seturgent(Client *c, int urg)
{
	XWMHints *wmh;

	c->isurgent = urg;
	if (!(wmh = XGetWMHints(dpy, c->win)))
		return;
	wmh->flags = urg ? (wmh->flags | XUrgencyHint) : (wmh->flags & ~XUrgencyHint);
	XSetWMHints(dpy, c->win, wmh);
	XFree(wmh);
}

void
showhide(Client *c)
{
	if (!c)
		return;
	if (ISVISIBLE(c)) {
		/* show clients top down */
		XMoveWindow(dpy, c->win, c->x, c->y);
		if ((!c->mon->lt[c->mon->sellt]->arrange || c->isfloating) && !c->isfullscreen)
			resize(c, c->x, c->y, c->w, c->h, 0);
		showhide(c->snext);
	} else {
		/* hide clients bottom up */
		showhide(c->snext);
		XMoveWindow(dpy, c->win, WIDTH(c) * -2, c->y);
	}
}

void
sigchld(int unused)
{
	pid_t pid;

	if (signal(SIGCHLD, sigchld) == SIG_ERR)
		die("can't install SIGCHLD handler:");
	while (0 < (pid = waitpid(-1, NULL, WNOHANG))) {
		pid_t *p, *lim;

		if (!(p = autostart_pids))
			continue;
		lim = &p[autostart_len];

		for (; p < lim; p++) {
			if (*p == pid) {
				*p = -1;
				break;
			}
		}

	}
}

void
sighup(int unused)
{
	Arg a = {.i = 1};
	quit(&a);
}

void
sigterm(int unused)
{
	Arg a = {.i = 0};
	quit(&a);
}

void
sigstatusbar(const Arg *arg)
{
	union sigval sv;

	if (!statussig)
		return;
	sv.sival_int = arg->i;
	if ((statuspid = getstatusbarpid()) <= 0)
		return;

	sigqueue(statuspid, SIGRTMIN+statussig, sv);
}

void
spawn(const Arg *arg)
{
	if (arg->v == dmenucmd || arg->v == drundmenucmd)
		dmenumon[0] = '0' + selmon->num;
	selmon->tagset[selmon->seltags] &= ~scratchtag;
	if (fork() == 0) {
		if (dpy)
			close(ConnectionNumber(dpy));
		setsid();
		execvp(((char **)arg->v)[0], (char **)arg->v);
		fprintf(stderr, "dwm: execvp %s", ((char **)arg->v)[0]);
		perror(" failed");
		exit(EXIT_SUCCESS);
	}
}

__attribute__((unused))
void
switchcol(const Arg *arg)
{
	Client *c, *t;
	int col = 0;
	int i;

	if (!selmon->sel)
	        return;
	for (i = 0, c = nexttiled(selmon->clients); c ;
		     c = nexttiled(c->next), i++) {
	        if (c == selmon->sel)
		         col = (i + 1) > selmon->nmaster;
	}
	if (i <= selmon->nmaster)
	        return;
	for (c = selmon->stack; c; c = c->snext) {
	        if (!ISVISIBLE(c))
	         continue;
       for (i = 0, t = nexttiled(selmon->clients); t && t != c;
       	             t = nexttiled(t->next), i++);
       if (t && (i + 1 > selmon->nmaster) != col) {
	         focus(c);
	         restack(selmon);
	         break;
	        }
	}
}

void
setclienttagprop(Client *c)
{
	long data[] = { (long) c->tags, (long) c->mon->num };
	XChangeProperty(dpy, c->win, netatom[NetClientInfo], XA_CARDINAL, 32,
			PropModeReplace, (unsigned char *) data, 2);
}

void
tag(const Arg *arg)
{
	Client *c;
	if (selmon->sel && arg->ui & TAGMASK) {
		c = selmon->sel;
		selmon->sel->tags = arg->ui & TAGMASK;
		setclienttagprop(c);
		focus(NULL);
		arrange(selmon);
	}
}

void
tagmon(const Arg *arg)
{
	if (!selmon->sel || !mons->next)
		return;
	sendmon(selmon->sel, dirtomon(arg->i));
}

void
tile(Monitor *m)
{
	unsigned int i, n, h, mw, my, ty;
	Client *c;

	for (n = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), n++);
	if (n == 0)
		return;

	if (m->pertag->drawwithgaps[m->pertag->curtag]) { /* draw with fullgaps logic */
	        if (n > m->nmaster)
	                mw = m->nmaster ? m->ww * m->mfact : 0;
	        else
	                mw = m->ww - m->pertag->gappx[m->pertag->curtag];
	        for (i = 0, my = ty = m->pertag->gappx[m->pertag->curtag], c = nexttiled(m->clients); c; c = nexttiled(c->next), i++)
	                if (i < m->nmaster) {
	                        h = (m->wh - my) / (MIN(n, m->nmaster) - i) - m->pertag->gappx[m->pertag->curtag];
	                        resize(c, m->wx + m->pertag->gappx[m->pertag->curtag], m->wy + my, mw - (2*c->bw) - m->pertag->gappx[m->pertag->curtag], h - (2*c->bw), 0);
	                        if (my + HEIGHT(c) + m->pertag->gappx[m->pertag->curtag] < m->wh)
	                                my += HEIGHT(c) + m->pertag->gappx[m->pertag->curtag];
	                } else {
	                        h = (m->wh - ty) / (n - i) - m->pertag->gappx[m->pertag->curtag];
	                        resize(c, m->wx + mw + m->pertag->gappx[m->pertag->curtag], m->wy + ty, m->ww - mw - (2*c->bw) - 2*m->pertag->gappx[m->pertag->curtag], h - (2*c->bw), 0);
	                        if (ty + HEIGHT(c) + m->pertag->gappx[m->pertag->curtag] < m->wh)
	                                ty += HEIGHT(c) + m->pertag->gappx[m->pertag->curtag];
	                }
	} else { /* draw with singularborders logic */
	        if (n > m->nmaster)
	                mw = m->nmaster ? m->ww * m->mfact : 0;
	        else
	                mw = m->ww;
	        for (i = my = ty = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), i++)
	                if (i < m->nmaster) {
	                        h = (m->wh - my) / (MIN(n, m->nmaster) - i);
	                        if (n == 1) /* might be able to combine this with the next case */
	                                resize(c, m->wx - c->bw, m->wy, m->ww, m->wh - c->bw, False);
				else if (n <= m->nmaster) /* 1 column */
	                                resize(c, m->wx - c->bw, m->wy + my, mw, h - c->bw, False);
				else
	                                resize(c, m->wx - c->bw, m->wy + my, mw - c->bw, h - c->bw, False);
	                        my += HEIGHT(c) - c->bw;
	                } else {
	                        h = (m->wh - ty) / (n - i);
	                        resize(c, m->wx + mw - c->bw, m->wy + ty, m->ww - mw, h - c->bw, False);
	                        ty += HEIGHT(c) - c->bw;
	                }
	}
}

void
tilewide(Monitor *m)
{
        unsigned int i, n, w, h, mw, mx, ty;
	Client *c;

	for (n = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), n++);
	if (n == 0)
		return;

	if (n > m->nmaster)
		mw = m->nmaster ? m->ww * m->mfact : 0;
	else
		mw = m->ww;
	for (i = mx = ty = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), i++)
		if (i < m->nmaster) {
		        w = (mw - mx) / (MIN(n, m->nmaster) - i);
		        resize(c, m->wx + mx, m->wy, w - (2*c->bw), (m->wh - ty) - (2*c->bw), 0);
		        if  (mx + WIDTH(c) < m->ww)
		                mx += WIDTH(c);
		} else {
			h = (m->wh - ty) / (n - i);
			resize(c, m->wx + mw, m->wy + ty, m->ww - mw - (2*c->bw), h - (2*c->bw), 0);
			if (ty + HEIGHT(c) < m->wh)
				ty += HEIGHT(c);
		}
}

void
togglebar(const Arg *arg)
{
	selmon->showbar = selmon->pertag->showbars[selmon->pertag->curtag] = !selmon->showbar;
	updatebarpos(selmon);
	resizebarwin(selmon);
	if (showsystray) {
		XWindowChanges wc;
		if (!selmon->showbar)
			wc.y = -bh;
		else if (selmon->showbar) {
			wc.y = 0;
			if (!selmon->topbar)
				wc.y = selmon->mh - bh;
		}
		XConfigureWindow(dpy, systray->win, CWY, &wc);
	}
	arrange(selmon);
}

void
togglefloating(const Arg *arg)
{
	if (!selmon->sel)
		return;
	if (selmon->sel->isfullscreen) /* no support for fullscreen windows */
		return;
	selmon->sel->isfloating = !selmon->sel->isfloating || selmon->sel->isfixed;
	if (selmon->sel->isfloating)
		resize(selmon->sel, selmon->sel->x, selmon->sel->y,
			selmon->sel->w, selmon->sel->h, 0);
	arrange(selmon);
}

void
togglefullscreen(const Arg *arg)
{
	if (selmon->sel){
		setfullscreen(selmon->sel, !selmon->sel->isfullscreen);
	}
}

void
togglescratch(const Arg *arg)
{
	Client *c;
	unsigned int found = 0;

	for (c = selmon->clients; c && !(found = c->tags & scratchtag); c = c->next);
	if (found) {
		unsigned int newtagset = selmon->tagset[selmon->seltags] ^ scratchtag;
		if (newtagset) {
			selmon->tagset[selmon->seltags] = newtagset;
			focus(NULL);
			arrange(selmon);
		}
		if (ISVISIBLE(c)) {
			focus(c);
			restack(selmon);
		}
	} else
		spawn(arg);
}

void
toggletag(const Arg *arg)
{
	unsigned int newtags;

	if (!selmon->sel)
		return;
	newtags = selmon->sel->tags ^ (arg->ui & TAGMASK);
	if (newtags) {
		selmon->sel->tags = newtags;
		setclienttagprop(selmon->sel);
		focus(NULL);
		arrange(selmon);
	}
}

void
toggleview(const Arg *arg)
{
	unsigned int newtagset = selmon->tagset[selmon->seltags] ^ (arg->ui & TAGMASK);
	int i;

	if (newtagset) {
		selmon->tagset[selmon->seltags] = newtagset;

		if (newtagset == ~0) {
			selmon->pertag->prevtag = selmon->pertag->curtag;
			selmon->pertag->curtag = 0;
		}

		/* test if the user did not select the same tag */
		if (!(newtagset & 1 << (selmon->pertag->curtag - 1))) {
			selmon->pertag->prevtag = selmon->pertag->curtag;
			for (i = 0; !(newtagset & 1 << i); i++) ;
			selmon->pertag->curtag = i + 1;
		}

		/* apply settings for this view */
		selmon->nmaster = selmon->pertag->nmasters[selmon->pertag->curtag];
		selmon->mfact = selmon->pertag->mfacts[selmon->pertag->curtag];
		selmon->sellt = selmon->pertag->sellts[selmon->pertag->curtag];
		selmon->lt[selmon->sellt] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt];
		selmon->lt[selmon->sellt^1] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt^1];

		if (selmon->showbar != selmon->pertag->showbars[selmon->pertag->curtag])
			togglebar(NULL);

		focus(NULL);
		arrange(selmon);
	}
}

void
unfocus(Client *c, int setfocus)
{
	if (!c)
		return;
	grabbuttons(c, 0);
	XSetWindowBorder(dpy, c->win, scheme[SchemeNorm][ColBorder].pixel);
	if (setfocus) {
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
	}
}

void
unmanage(Client *c, int destroyed)
{
	Monitor *m = c->mon;
	XWindowChanges wc;
	int fullscreen = (selmon->sel == c && selmon->sel->isfullscreen)?1:0;

	if (c->swallowing) {
		unswallow(c);
		return;
	}

	Client *s = swallowingclient(c->win);
	if (s) {
		free(s->swallowing);
		s->swallowing = NULL;
		arrange(m);
		focus(NULL);
		return;
	}

	detach(c);
	detachstack(c);
	if (!destroyed) {
		wc.border_width = c->oldbw;
		XGrabServer(dpy); /* avoid race conditions */
		XSetErrorHandler(xerrordummy);
		XConfigureWindow(dpy, c->win, CWBorderWidth, &wc); /* restore border */
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		setclientstate(c, WithdrawnState);
		XSync(dpy, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dpy);
	}
	free(c);

	if (fullscreen)
		togglefullscreen(NULL);

	if (!s) {
		arrange(m);
		focus(NULL);
		updateclientlist();
	}
}

void
unmapnotify(XEvent *e)
{
	Client *c;
	XUnmapEvent *ev = &e->xunmap;

	if ((c = wintoclient(ev->window))) {
		if (ev->send_event)
			setclientstate(c, WithdrawnState);
		else
			unmanage(c, 0);
	}
	else if ((c = wintosystrayicon(ev->window))) {
		/* KLUDGE! sometimes icons occasionally unmap their windows, but do
		 * _not_ destroy them. We map those windows back */
		XMapRaised(dpy, c->win);
		updatesystray();
	}
}

void
updatebars(void)
{
	unsigned int w;
	Monitor *m;
	XSetWindowAttributes wa = {
		.override_redirect = True,
		.background_pixmap = ParentRelative,
		.event_mask = ButtonPressMask|ExposureMask
	};
	XClassHint ch = {"dwm", "dwm"};
	for (m = mons; m; m = m->next) {
		if (m->barwin)
			continue;
		w = m->ww;
		if (showsystray && m == systraytomon(m))
			w -= getsystraywidth();
		m->barwin = XCreateWindow(dpy, root, m->wx, m->by, w, bh, 0, DefaultDepth(dpy, screen),
				CopyFromParent, DefaultVisual(dpy, screen),
				CWOverrideRedirect|CWBackPixmap|CWEventMask, &wa);
		XDefineCursor(dpy, m->barwin, cursor[CurNormal]->cursor);
		if (showsystray && m == systraytomon(m))
			XMapRaised(dpy, systray->win);
		XMapRaised(dpy, m->barwin);
		XSetClassHint(dpy, m->barwin, &ch);
	}
}

void
updatebarpos(Monitor *m)
{
	m->wy = m->my;
	m->wh = m->mh;
	if (m->showbar) {
		m->wh -= bh;
		m->by = m->topbar ? m->wy : m->wy + m->wh;
		m->wy = m->topbar ? m->wy + bh : m->wy;
	} else
		m->by = -bh;
}

void
updateclientlist()
{
	Client *c;
	Monitor *m;

	XDeleteProperty(dpy, root, netatom[NetClientList]);
	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next)
			XChangeProperty(dpy, root, netatom[NetClientList],
				XA_WINDOW, 32, PropModeAppend,
				(unsigned char *) &(c->win), 1);
}

int
updategeom(void)
{
	int dirty = 0;

#ifdef XINERAMA
	if (XineramaIsActive(dpy)) {
		int i, j, n, nn;
		Client *c;
		Monitor *m;
		XineramaScreenInfo *info = XineramaQueryScreens(dpy, &nn);
		XineramaScreenInfo *unique = NULL;

		for (n = 0, m = mons; m; m = m->next, n++);
		/* only consider unique geometries as separate screens */
		unique = ecalloc(nn, sizeof(XineramaScreenInfo));
		for (i = 0, j = 0; i < nn; i++)
			if (isuniquegeom(unique, j, &info[i]))
				memcpy(&unique[j++], &info[i], sizeof(XineramaScreenInfo));
		XFree(info);
		nn = j;
		if (n <= nn) { /* new monitors available */
			for (i = 0; i < (nn - n); i++) {
				for (m = mons; m && m->next; m = m->next);
				if (m)
					m->next = createmon();
				else
					mons = createmon();
			}
			for (i = 0, m = mons; i < nn && m; m = m->next, i++)
				if (i >= n
				|| unique[i].x_org != m->mx || unique[i].y_org != m->my
				|| unique[i].width != m->mw || unique[i].height != m->mh)
				{
					dirty = 1;
					m->num = i;
					m->mx = m->wx = unique[i].x_org;
					m->my = m->wy = unique[i].y_org;
					m->mw = m->ww = unique[i].width;
					m->mh = m->wh = unique[i].height;
					updatebarpos(m);
				}
		} else { /* less monitors available nn < n */
			for (i = nn; i < n; i++) {
				for (m = mons; m && m->next; m = m->next);
				while ((c = m->clients)) {
					dirty = 1;
					m->clients = c->next;
					detachstack(c);
					c->mon = mons;
					attachaside(c);
					attachstack(c);
				}
				if (m == selmon)
					selmon = mons;
				cleanupmon(m);
			}
		}
		free(unique);
	} else
#endif /* XINERAMA */
	{ /* default monitor setup */
		if (!mons)
			mons = createmon();
		if (mons->mw != sw || mons->mh != sh) {
			dirty = 1;
			mons->mw = mons->ww = sw;
			mons->mh = mons->wh = sh;
			updatebarpos(mons);
		}
	}
	if (dirty) {
		selmon = mons;
		selmon = wintomon(root);
	}
	return dirty;
}

void
updatemotifhints(Client *c)
{
	Atom real;
	int format;
	unsigned char *p = NULL;
	unsigned long n, extra;
	unsigned long *motif;
	int width, height;

	if (!decorhints)
		return;

	if (XGetWindowProperty(dpy, c->win, motifatom, 0L, 5L, False, motifatom,
	                       &real, &format, &n, &extra, &p) == Success && p != NULL) {
		motif = (unsigned long*)p;
		if (motif[MWM_HINTS_FLAGS_FIELD] & MWM_HINTS_DECORATIONS) {
			width = WIDTH(c);
			height = HEIGHT(c);

			if (motif[MWM_HINTS_DECORATIONS_FIELD] & MWM_DECOR_ALL ||
			    motif[MWM_HINTS_DECORATIONS_FIELD] & MWM_DECOR_BORDER ||
			    motif[MWM_HINTS_DECORATIONS_FIELD] & MWM_DECOR_TITLE)
				c->bw = c->oldbw = borderpx;
			else
				c->bw = c->oldbw = 0;

			resize(c, c->x, c->y, width - (2*c->bw), height - (2*c->bw), 0);
		}
		XFree(p);
	}
}

void
updatenumlockmask(void)
{
	unsigned int i, j;
	XModifierKeymap *modmap;

	numlockmask = 0;
	modmap = XGetModifierMapping(dpy);
	for (i = 0; i < 8; i++)
		for (j = 0; j < modmap->max_keypermod; j++)
			if (modmap->modifiermap[i * modmap->max_keypermod + j]
				== XKeysymToKeycode(dpy, XK_Num_Lock))
				numlockmask = (1 << i);
	XFreeModifiermap(modmap);
}

void
updatesizehints(Client *c)
{
	long msize;
	XSizeHints size;

	if (!XGetWMNormalHints(dpy, c->win, &size, &msize))
		/* size is uninitialized, ensure that size.flags aren't used */
		size.flags = PSize;
	if (size.flags & PBaseSize) {
		c->basew = size.base_width;
		c->baseh = size.base_height;
	} else if (size.flags & PMinSize) {
		c->basew = size.min_width;
		c->baseh = size.min_height;
	} else
		c->basew = c->baseh = 0;
	if (size.flags & PResizeInc) {
		c->incw = size.width_inc;
		c->inch = size.height_inc;
	} else
		c->incw = c->inch = 0;
	if (size.flags & PMaxSize) {
		c->maxw = size.max_width;
		c->maxh = size.max_height;
	} else
		c->maxw = c->maxh = 0;
	if (size.flags & PMinSize) {
		c->minw = size.min_width;
		c->minh = size.min_height;
	} else if (size.flags & PBaseSize) {
		c->minw = size.base_width;
		c->minh = size.base_height;
	} else
		c->minw = c->minh = 0;
	if (size.flags & PAspect) {
		c->mina = (float)size.min_aspect.y / size.min_aspect.x;
		c->maxa = (float)size.max_aspect.x / size.max_aspect.y;
	} else
		c->maxa = c->mina = 0.0;
	c->isfixed = (c->maxw && c->maxh && c->maxw == c->minw && c->maxh == c->minh);
}

void
updatestatus(void)
{
	Monitor *m;
	if (!gettextprop(root, XA_WM_NAME, stext, sizeof(stext)))
		strcpy(stext, "dwm-"VERSION);
	for(m = mons; m; m = m->next)
		drawbar(m);
	updatesystray();
}


void
updatesystrayicongeom(Client *i, int w, int h)
{
	if (i) {
		i->h = bh;
		if (w == h)
			i->w = bh;
		else if (h == bh)
			i->w = w;
		else
			i->w = (int) ((float)bh * ((float)w / (float)h));
		applysizehints(i, &(i->x), &(i->y), &(i->w), &(i->h), False);
		/* force icons into the systray dimensions if they don't want to */
		if (i->h > bh) {
			if (i->w == i->h)
				i->w = bh;
			else
				i->w = (int) ((float)bh * ((float)i->w / (float)i->h));
			i->h = bh;
		}
	}
}

void
updatesystrayiconstate(Client *i, XPropertyEvent *ev)
{
	long flags;
	int code = 0;

	if (!showsystray || !i || ev->atom != xatom[XembedInfo] ||
			!(flags = getatomprop(i, xatom[XembedInfo])))
		return;

	if (flags & XEMBED_MAPPED && !i->tags) {
		i->tags = 1;
		code = XEMBED_WINDOW_ACTIVATE;
		XMapRaised(dpy, i->win);
		setclientstate(i, NormalState);
	}
	else if (!(flags & XEMBED_MAPPED) && i->tags) {
		i->tags = 0;
		code = XEMBED_WINDOW_DEACTIVATE;
		XUnmapWindow(dpy, i->win);
		setclientstate(i, WithdrawnState);
	}
	else
		return;
	sendevent(i->win, xatom[Xembed], StructureNotifyMask, CurrentTime, code, 0,
			systray->win, XEMBED_EMBEDDED_VERSION);
}

void
updatesystray(void)
{
	XSetWindowAttributes wa;
	XWindowChanges wc;
	Client *i;
	Monitor *m = systraytomon(NULL);
	unsigned int x = m->mx + m->mw;
	unsigned int sw = TEXTW(stext) - lrpad + systrayspacing;
	unsigned int w = 1;

	if (!showsystray)
		return;
	if (systrayonleft)
		x -= sw + lrpad / 2;
	if (!systray) {
		/* init systray */
		if (!(systray = (Systray *)calloc(1, sizeof(Systray))))
			die("fatal: could not malloc() %u bytes\n", sizeof(Systray));
		systray->win = XCreateSimpleWindow(dpy, root, x, m->by, w, bh, 0, 0, scheme[SchemeSel][ColBg].pixel);
		wa.event_mask        = ButtonPressMask | ExposureMask;
		wa.override_redirect = True;
		wa.background_pixel  = scheme[SchemeNorm][ColBg].pixel;
		XSelectInput(dpy, systray->win, SubstructureNotifyMask);
		XChangeProperty(dpy, systray->win, netatom[NetSystemTrayOrientation], XA_CARDINAL, 32,
				PropModeReplace, (unsigned char *)&netatom[NetSystemTrayOrientationHorz], 1);
		XChangeWindowAttributes(dpy, systray->win, CWEventMask|CWOverrideRedirect|CWBackPixel, &wa);
		XMapRaised(dpy, systray->win);
		XSetSelectionOwner(dpy, netatom[NetSystemTray], systray->win, CurrentTime);
		if (XGetSelectionOwner(dpy, netatom[NetSystemTray]) == systray->win) {
			sendevent(root, xatom[Manager], StructureNotifyMask, CurrentTime, netatom[NetSystemTray], systray->win, 0, 0);
			XSync(dpy, False);
		}
		else {
			fprintf(stderr, "dwm: unable to obtain system tray.\n");
			free(systray);
			systray = NULL;
			return;
		}
	}
	for (w = 0, i = systray->icons; i; i = i->next) {
		/* make sure the background color stays the same */
		wa.background_pixel  = scheme[SchemeNorm][ColBg].pixel;
		XChangeWindowAttributes(dpy, i->win, CWBackPixel, &wa);
		XMapRaised(dpy, i->win);
		w += systrayspacing;
		i->x = w;
		XMoveResizeWindow(dpy, i->win, i->x, 0, i->w, i->h);
		w += i->w;
		if (i->mon != m)
			i->mon = m;
	}
	w = w ? w + systrayspacing : 1;
	x -= w;
	XMoveResizeWindow(dpy, systray->win, x, m->by, w, bh);
	wc.x = x; wc.y = m->by; wc.width = w; wc.height = bh;
	wc.stack_mode = Above; wc.sibling = m->barwin;
	XConfigureWindow(dpy, systray->win, CWX|CWY|CWWidth|CWHeight|CWSibling|CWStackMode, &wc);
	XMapWindow(dpy, systray->win);
	XMapSubwindows(dpy, systray->win);
	/* redraw background */
	XSetForeground(dpy, drw->gc, scheme[SchemeNorm][ColBg].pixel);
	XFillRectangle(dpy, systray->win, drw->gc, 0, 0, w, bh);
	XSync(dpy, False);
}

void
updatetitle(Client *c)
{
	char oldname[sizeof(c->name)];
	strcpy(oldname, c->name);

	if (!gettextprop(c->win, netatom[NetWMName], c->name, sizeof c->name))
		gettextprop(c->win, XA_WM_NAME, c->name, sizeof c->name);
	if (c->name[0] == '\0') /* hack to mark broken clients */
		strcpy(c->name, broken);

	for (Monitor *m = mons; m; m = m->next) {
		if (m->sel == c && strcmp(oldname, c->name) != 0)
			ipc_focused_title_change_event(m->num, c->win, oldname, c->name);
	}
}

void
updatewindowtype(Client *c)
{
	Atom state = getatomprop(c, netatom[NetWMState]);
	Atom wtype = getatomprop(c, netatom[NetWMWindowType]);

	if (state == netatom[NetWMFullscreen])
		setfullscreen(c, 1);
	if (wtype == netatom[NetWMWindowTypeDialog])
		c->isfloating = 1;
}

void
updatewmhints(Client *c)
{
	XWMHints *wmh;

	if ((wmh = XGetWMHints(dpy, c->win))) {
		if (c == selmon->sel && wmh->flags & XUrgencyHint) {
			wmh->flags &= ~XUrgencyHint;
			XSetWMHints(dpy, c->win, wmh);
		} else
			c->isurgent = (wmh->flags & XUrgencyHint) ? 1 : 0;
		if (wmh->flags & InputHint)
			c->neverfocus = !wmh->input;
		else
			c->neverfocus = 0;
		XFree(wmh);
	}
}

void
view(const Arg *arg)
{
	int i;
	unsigned int tmptag;

	if ((arg->ui & TAGMASK) == selmon->tagset[selmon->seltags])
		return;
	selmon->seltags ^= 1; /* toggle sel tagset */
	if (arg->ui & TAGMASK) {
		selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;
		selmon->pertag->prevtag = selmon->pertag->curtag;

		if (arg->ui == ~0)
			selmon->pertag->curtag = 0;
		else {
			for (i = 0; !(arg->ui & 1 << i); i++) ;
			selmon->pertag->curtag = i + 1;
		}
	} else {
		tmptag = selmon->pertag->prevtag;
		selmon->pertag->prevtag = selmon->pertag->curtag;
		selmon->pertag->curtag = tmptag;
	}

	selmon->nmaster = selmon->pertag->nmasters[selmon->pertag->curtag];
	selmon->mfact = selmon->pertag->mfacts[selmon->pertag->curtag];
	selmon->sellt = selmon->pertag->sellts[selmon->pertag->curtag];
	selmon->lt[selmon->sellt] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt];
	selmon->lt[selmon->sellt^1] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt^1];

	if (selmon->showbar != selmon->pertag->showbars[selmon->pertag->curtag])
		togglebar(NULL);

	focus(NULL);
	arrange(selmon);
}

pid_t
winpid(Window w)
{

	pid_t result = 0;

#ifdef __linux__
	xcb_res_client_id_spec_t spec = {0};
	spec.client = w;
	spec.mask = XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID;

	xcb_generic_error_t *e = NULL;
	xcb_res_query_client_ids_cookie_t c = xcb_res_query_client_ids(xcon, 1, &spec);
	xcb_res_query_client_ids_reply_t *r = xcb_res_query_client_ids_reply(xcon, c, &e);

	if (!r)
		return (pid_t)0;

	xcb_res_client_id_value_iterator_t i = xcb_res_query_client_ids_ids_iterator(r);
	for (; i.rem; xcb_res_client_id_value_next(&i)) {
		spec = i.data->spec;
		if (spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) {
			uint32_t *t = xcb_res_client_id_value_value(i.data);
			result = *t;
			break;
		}
	}

	free(r);

	if (result == (pid_t)-1)
		result = 0;

#endif /* __linux__ */

#ifdef __OpenBSD__
        Atom type;
        int format;
        unsigned long len, bytes;
        unsigned char *prop;
        pid_t ret;

        if (XGetWindowProperty(dpy, w, XInternAtom(dpy, "_NET_WM_PID", 0), 0, 1, False, AnyPropertyType, &type, &format, &len, &bytes, &prop) != Success || !prop)
               return 0;

        ret = *(pid_t*)prop;
        XFree(prop);
        result = ret;

#endif /* __OpenBSD__ */
	return result;
}

pid_t
getparentprocess(pid_t p)
{
	unsigned int v = 0;

#ifdef __linux__
	FILE *f;
	char buf[256];
	snprintf(buf, sizeof(buf) - 1, "/proc/%u/stat", (unsigned)p);

	if (!(f = fopen(buf, "r")))
		return 0;

	if (fscanf(f, "%*u %*s %*c %u", &v)){};
	fclose(f);
#endif /* __linux__*/

#ifdef __OpenBSD__
	int n;
	kvm_t *kd;
	struct kinfo_proc *kp;

	kd = kvm_openfiles(NULL, NULL, NULL, KVM_NO_FILES, NULL);
	if (!kd)
		return 0;

	kp = kvm_getprocs(kd, KERN_PROC_PID, p, sizeof(*kp), &n);
	v = kp->p_ppid;
#endif /* __OpenBSD__ */

	return (pid_t)v;
}

int
isdescprocess(pid_t p, pid_t c)
{
	while (p != c && c != 0)
		c = getparentprocess(c);

	return (int)c;
}

Client *
termforwin(const Client *w)
{
	Client *c;
	Monitor *m;

	if (!w->pid || w->isterminal)
		return NULL;

	for (m = mons; m; m = m->next) {
		for (c = m->clients; c; c = c->next) {
			if (c->isterminal && !c->swallowing && c->pid && isdescprocess(c->pid, w->pid))
				return c;
		}
	}

	return NULL;
}

Client *
swallowingclient(Window w)
{
	Client *c;
	Monitor *m;

	for (m = mons; m; m = m->next) {
		for (c = m->clients; c; c = c->next) {
			if (c->swallowing && c->swallowing->win == w)
				return c;
		}
	}

	return NULL;
}

Client *
wintoclient(Window w)
{
	Client *c;
	Monitor *m;

	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next)
			if (c->win == w)
				return c;
	return NULL;
}

Client *
wintosystrayicon(Window w) {
	Client *i = NULL;

	if (!showsystray || !w)
		return i;
	for (i = systray->icons; i && i->win != w; i = i->next) ;
	return i;
}

Monitor *
wintomon(Window w)
{
	int x, y;
	Client *c;
	Monitor *m;

	if (w == root && getrootptr(&x, &y))
		return recttomon(x, y, 1, 1);
	for (m = mons; m; m = m->next)
		if (w == m->barwin)
			return m;
	if ((c = wintoclient(w)))
		return c->mon;
	return selmon;
}

/* There's no way to check accesses to destroyed windows, thus those cases are
 * ignored (especially on UnmapNotify's). Other types of errors call Xlibs
 * default error handler, which may call exit. */
int
xerror(Display *dpy, XErrorEvent *ee)
{
	if (ee->error_code == BadWindow
	|| (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch)
	|| (ee->request_code == X_PolyText8 && ee->error_code == BadDrawable)
	|| (ee->request_code == X_PolyFillRectangle && ee->error_code == BadDrawable)
	|| (ee->request_code == X_PolySegment && ee->error_code == BadDrawable)
	|| (ee->request_code == X_ConfigureWindow && ee->error_code == BadMatch)
	|| (ee->request_code == X_GrabButton && ee->error_code == BadAccess)
	|| (ee->request_code == X_GrabKey && ee->error_code == BadAccess)
	|| (ee->request_code == X_CopyArea && ee->error_code == BadDrawable))
		return 0;
	fprintf(stderr, "dwm: fatal error: request code=%d, error code=%d\n",
		ee->request_code, ee->error_code);
	return xerrorxlib(dpy, ee); /* may call exit */
}

int
xerrordummy(Display *dpy, XErrorEvent *ee)
{
	return 0;
}

/* Startup Error handler to check if another window manager
 * is already running. */
int
xerrorstart(Display *dpy, XErrorEvent *ee)
{
	die("dwm: another window manager is already running");
	return -1;
}

Monitor *
systraytomon(Monitor *m) {
	Monitor *t;
	int i, n;
	if(!systraypinning) {
		if(!m)
			return selmon;
		return m == selmon ? m : NULL;
	}
	for(n = 1, t = mons; t && t->next; n++, t = t->next) ;
	for(i = 1, t = mons; t && t->next && i < systraypinning; i++, t = t->next) ;
	if(systraypinningfailfirst && n < systraypinning)
		return mons;
	return t;
}

void
xrdb(const Arg *arg)
{
	loadxrdb();
	int i;
	for (i = 0; i < LENGTH(colors); i++)
		scheme[i] = drw_scm_create(drw, colors[i], 3);
	setborderpx();
	focus(NULL);
	arrange(NULL);
}

void
zoom(const Arg *arg)
{
	Client *c = selmon->sel;

	if (!selmon->lt[selmon->sellt]->arrange
	|| (selmon->sel && selmon->sel->isfloating))
		return;
	if (c == nexttiled(selmon->clients))
		if (!c || !(c = nexttiled(c->next)))
			return;
	pop(c);
}

int
main(int argc, char *argv[])
{
	if (argc == 2 && !strcmp("-v", argv[1]))
		die("dwm-"VERSION);
	else if (argc != 1)
		die("usage: dwm [-v]");
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	if (!(dpy = XOpenDisplay(NULL)))
		die("dwm: cannot open display");
	if (!(xcon = XGetXCBConnection(dpy)))
		die("dwm: cannot get xcb connection\n");
	checkotherwm();
	autostart_exec();
        XrmInitialize();
        loadxrdb();
	setup();
#ifdef __OpenBSD__
	if (pledge("stdio rpath proc exec ps", NULL) == -1)
		die("pledge");
#endif /* __OpenBSD__ */
	scan();
	run();
	if(restart) execvp(argv[0], argv);
	cleanup();
	XCloseDisplay(dpy);
	return EXIT_SUCCESS;
}

void
centeredmaster(Monitor *m)
{
	unsigned int i, n, h, mw, mx, my, oty, ety, tw;
	Client *c;

	/* count number of clients in the selected monitor */
	for (n = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), n++);
	if (n == 0)
		return;

	/* initialize areas */
	mw = m->ww;
	mx = 0;
	my = 0;
	tw = mw;

	if (n > m->nmaster) {
		/* go mfact box in the center if more than nmaster clients */
		mw = m->nmaster ? m->ww * m->mfact : 0;
		tw = m->ww - mw;

		if (n - m->nmaster > 1) {
			/* only one client */
			mx = (m->ww - mw) / 2;
			tw = (m->ww - mw) / 2;
		}
	}

	oty = 0;
	ety = 0;
	for (i = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), i++)
	if (i < m->nmaster) {
		/* nmaster clients are stacked vertically, in the center
		 * of the screen */
		h = (m->wh - my) / (MIN(n, m->nmaster) - i);
		resize(c, m->wx + mx, m->wy + my, mw - (2*c->bw),
		       h - (2*c->bw), 0);
		my += HEIGHT(c);
	} else {
		/* stack clients are stacked vertically */
		if ((i - m->nmaster) % 2 ) {
			h = (m->wh - ety) / ( (1 + n - i) / 2);
			resize(c, m->wx, m->wy + ety, tw - (2*c->bw),
			       h - (2*c->bw), 0);
			ety += HEIGHT(c);
		} else {
			h = (m->wh - oty) / ((1 + n - i) / 2);
			resize(c, m->wx + mx + mw, m->wy + oty,
			       tw - (2*c->bw), h - (2*c->bw), 0);
			oty += HEIGHT(c);
		}
	}
}

void
centeredfloatingmaster(Monitor *m)
{
	unsigned int i, n, w, mh, mw, mx, mxo, my, myo, tx;
	Client *c;

	/* count number of clients in the selected monitor */
	for (n = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), n++);
	if (n == 0)
		return;

	/* initialize nmaster area */
	if (n > m->nmaster) {
		/* go mfact box in the center if more than nmaster clients */
		if (m->ww > m->wh) {
			mw = m->nmaster ? m->ww * m->mfact : 0;
			mh = m->nmaster ? m->wh * 0.9 : 0;
		} else {
			mh = m->nmaster ? m->wh * m->mfact : 0;
			mw = m->nmaster ? m->ww * 0.9 : 0;
		}
		mx = mxo = (m->ww - mw) / 2;
		my = myo = (m->wh - mh) / 2;
	} else {
		/* go fullscreen if all clients are in the master area */
		mh = m->wh;
		mw = m->ww;
		mx = mxo = 0;
		my = myo = 0;
	}

	for(i = tx = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), i++)
	if (i < m->nmaster) {
		/* nmaster clients are stacked horizontally, in the center
		 * of the screen */
		w = (mw + mxo - mx) / (MIN(n, m->nmaster) - i);
		resize(c, m->wx + mx, m->wy + my, w - (2*c->bw),
		       mh - (2*c->bw), 0);
		mx += WIDTH(c);
	} else {
		/* stack clients are stacked horizontally */
		w = (m->ww - tx) / (n - i);
		resize(c, m->wx + tx, m->wy, w - (2*c->bw),
		       m->wh - (2*c->bw), 0);
		tx += WIDTH(c);
	}
}
