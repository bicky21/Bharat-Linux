/*
 * bharat-xwm - the smallest window manager: one window, full screen.
 *
 * Runs inside the X11 screen that x11-session starts for Tor Browser.
 * Every main window is made to fill the whole screen and gets the
 * keyboard; dialogs (save file, settings ...) keep their own size and
 * are centred. There are no title bars: Tor Browser closes with its
 * menu (Quit), Ctrl+Q or Alt+F4, and then the Bharat desktop comes back.
 *
 * Unlike the rest of Bharat Linux, this program uses the X11 library
 * (libX11), because it lives in the X11 world of Tor Browser.
 */

#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

static Display *dpy;
static int sw, sh;

/* the X server reports an error when a window vanished first: ignore */
static int ignore_errors(Display *d, XErrorEvent *e) {

    (void)d; (void)e;
    return 0;
}

static int is_dialog(Window w) {

    Window parent = None;

    return XGetTransientForHint(dpy, w, &parent) && parent != None;
}

/* full screen, unless the window cannot be that small */
static void fill_screen(Window w) {

    XSizeHints h;
    long supplied;
    int width = sw, height = sh;

    if (XGetWMNormalHints(dpy, w, &h, &supplied) && (h.flags & PMinSize)) {

        if (h.min_width > width) width = h.min_width;
        if (h.min_height > height) height = h.min_height;
    }

    XMoveResizeWindow(dpy, w, 0, 0, (unsigned)width, (unsigned)height);
}

static void place(Window w, int width, int height) {

    if (!is_dialog(w)) {

        fill_screen(w);
        return;
    }

    if (width > sw) width = sw;
    if (height > sh) height = sh;

    XMoveResizeWindow(dpy, w, (sw - width) / 2, (sh - height) / 2, (unsigned)width, (unsigned)height);
}

/* Alt+F4: ask the window on top (the one with the keyboard) to close */
static void close_focused(void) {

    Window w;
    int revert;

    XGetInputFocus(dpy, &w, &revert);

    if (w == None || w == PointerRoot || w == DefaultRootWindow(dpy))
        return;

    /* the focus may be inside a child window: find the top-level one */
    for (;;) {

        Window r, parent, *kids = NULL;
        unsigned n;

        if (!XQueryTree(dpy, w, &r, &parent, &kids, &n))
            return;

        if (kids)
            XFree(kids);

        if (parent == r)
            break;

        w = parent;
    }

    XEvent ev = {0};

    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = XInternAtom(dpy, "WM_PROTOCOLS", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, w, False, NoEventMask, &ev);
}

int main(void) {

    dpy = XOpenDisplay(NULL);

    if (!dpy) {

        fprintf(stderr, "bharat-xwm: cannot open the display\n");
        return 1;
    }

    int scr = DefaultScreen(dpy);
    Window root = RootWindow(dpy, scr);

    sw = DisplayWidth(dpy, scr);
    sh = DisplayHeight(dpy, scr);

    XSetErrorHandler(ignore_errors);

    /* we decide where windows go */
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask);
    XSetWindowBackground(dpy, root, 0x0E1B30);
    XDefineCursor(dpy, root, XCreateFontCursor(dpy, XC_left_ptr));
    XClearWindow(dpy, root);

    /* Alt+F4, with or without Num Lock / Caps Lock */
    KeyCode f4 = XKeysymToKeycode(dpy, XK_F4);
    unsigned extra[] = {0, Mod2Mask, LockMask, Mod2Mask | LockMask};

    for (int i = 0; i < 4; i++)
        XGrabKey(dpy, f4, Mod1Mask | extra[i], root, True, GrabModeAsync, GrabModeAsync);

    XSync(dpy, False);

    for (;;) {

        XEvent ev;

        XNextEvent(dpy, &ev);

        if (ev.type == KeyPress) {

            close_focused();
        }
        else if (ev.type == MapRequest) {

            Window w = ev.xmaprequest.window;
            XWindowAttributes a;

            if (XGetWindowAttributes(dpy, w, &a))
                place(w, a.width, a.height);

            XMapRaised(dpy, w);
            XSetInputFocus(dpy, w, RevertToPointerRoot, CurrentTime);
        }
        else if (ev.type == ConfigureRequest) {

            XConfigureRequestEvent *c = &ev.xconfigurerequest;

            if (is_dialog(c->window)) {

                place(c->window, c->width, c->height);
            }
            else {

                /* main windows stay full screen, whatever they ask */
                fill_screen(c->window);
            }
        }
        else if (ev.type == DestroyNotify || ev.type == UnmapNotify) {

            /* give the keyboard back to whatever window is left on top */
            Window r, p, *kids = NULL;
            unsigned n = 0;

            if (XQueryTree(dpy, root, &r, &p, &kids, &n)) {

                for (int i = (int)n - 1; i >= 0; i--) {

                    XWindowAttributes a;

                    if (XGetWindowAttributes(dpy, kids[i], &a) && a.map_state == IsViewable &&
                        !a.override_redirect) {

                        XSetInputFocus(dpy, kids[i], RevertToPointerRoot, CurrentTime);
                        break;
                    }
                }

                if (kids)
                    XFree(kids);
            }
        }
    }
}
