#include "platform/x11_window.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

#include <cstdio>
#include <cstring>

namespace hominka {

// Усе, що від X11. Живе лише тут — див. коментар у заголовку.
struct X11Window::Impl {
    Display* dpy = nullptr;
    int screen = 0;
    Window win = 0;
    Visual* visual = nullptr;
    Colormap cmap = 0;
    GC gc = nullptr;
    XImage* image = nullptr;          // обгортка над нашим буфером, без копії
    int depth = 32;
    Atom wm_delete = 0;
};

X11Window::X11Window() : p_(new Impl()) {}
X11Window::~X11Window() { destroy(); }

bool X11Window::ok() const { return p_->dpy != nullptr && p_->win != 0; }

bool X11Window::create(int x, int y, int w, int h, const char* title) {
    p_->dpy = XOpenDisplay(nullptr);
    if (!p_->dpy) return false;
    p_->screen = DefaultScreen(p_->dpy);
    title_ = title ? title : "Hominka";

    // 32-бітний ARGB-візуал: без нього прозорого тла не буде взагалі.
    XVisualInfo vi{};
    if (!XMatchVisualInfo(p_->dpy, p_->screen, 32, TrueColor, &vi)) {
        XCloseDisplay(p_->dpy);
        p_->dpy = nullptr;
        return false;
    }
    p_->visual = vi.visual;
    p_->depth = vi.depth;

    p_->cmap = XCreateColormap(p_->dpy, RootWindow(p_->dpy, p_->screen), p_->visual, AllocNone);

    XSetWindowAttributes attr{};
    attr.colormap = p_->cmap;
    attr.background_pixel = 0;         // прозорий, а не «колір за замовчуванням»
    attr.border_pixel = 0;
    // override_redirect: віконний менеджер це вікно не оформлює, не переносить
    // між робочими столами й не кладе в список задач. Саме те, що треба
    // оверлею — і саме тому перетягування доводиться робити самим.
    attr.override_redirect = True;
    attr.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask | StructureNotifyMask |
                      EnterWindowMask | LeaveWindowMask;

    x_ = x; y_ = y; w_ = w > 0 ? w : 1; h_ = h > 0 ? h : 1;
    p_->win = XCreateWindow(p_->dpy, RootWindow(p_->dpy, p_->screen), x_, y_, w_, h_, 0,
                         p_->depth, InputOutput, p_->visual,
                         CWColormap | CWBackPixel | CWBorderPixel |
                         CWOverrideRedirect | CWEventMask, &attr);
    if (!p_->win) {
        XCloseDisplay(p_->dpy);
        p_->dpy = nullptr;
        return false;
    }

    XStoreName(p_->dpy, p_->win, title_.c_str());
    p_->wm_delete = XInternAtom(p_->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(p_->dpy, p_->win, &p_->wm_delete, 1);
    apply_above();

    XGCValues gcv{};
    p_->gc = XCreateGC(p_->dpy, p_->win, 0, &gcv);

    int shape_ev = 0, shape_err = 0;
    have_shape_ = XShapeQueryExtension(p_->dpy, &shape_ev, &shape_err) == True;

    XFlush(p_->dpy);
    return true;
}

void X11Window::apply_above() {
    // Навіть при override-redirect підказуємо композитору намір: деякі
    // середовища за цими властивостями вирішують, куди класти вікно.
    Atom above = XInternAtom(p_->dpy, "_NET_WM_STATE_ABOVE", False);
    Atom state = XInternAtom(p_->dpy, "_NET_WM_STATE", False);
    XChangeProperty(p_->dpy, p_->win, state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char*)&above, 1);

    Atom type = XInternAtom(p_->dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom dock = XInternAtom(p_->dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    XChangeProperty(p_->dpy, p_->win, type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char*)&dock, 1);
}

void X11Window::destroy() {
    if (!p_->dpy) return;
    if (p_->image) {
        // XImage лише обгортає наш буфер — щоб X його не звільняв.
        p_->image->data = nullptr;
        XDestroyImage(p_->image);
        p_->image = nullptr;
    }
    if (p_->gc) { XFreeGC(p_->dpy, p_->gc); p_->gc = nullptr; }
    if (p_->win) { XDestroyWindow(p_->dpy, p_->win); p_->win = 0; }
    if (p_->cmap) { XFreeColormap(p_->dpy, p_->cmap); p_->cmap = 0; }
    XCloseDisplay(p_->dpy);
    p_->dpy = nullptr;
    shown_ = false;
}

void X11Window::show() {
    if (!ok() || shown_) return;
    XMapRaised(p_->dpy, p_->win);
    XFlush(p_->dpy);
    shown_ = true;
}

void X11Window::hide() {
    if (!ok() || !shown_) return;
    XUnmapWindow(p_->dpy, p_->win);
    XFlush(p_->dpy);
    shown_ = false;
}

void X11Window::set_geometry(int x, int y, int w, int h) {
    if (!ok()) return;
    if (w <= 0) w = w_;
    if (h <= 0) h = h_;
    if (x == x_ && y == y_ && w == w_ && h == h_) return;
    x_ = x; y_ = y; w_ = w; h_ = h;
    XMoveResizeWindow(p_->dpy, p_->win, x_, y_, (unsigned)w_, (unsigned)h_);
    // Розмір змінився — стара обгортка вже не про цей буфер.
    if (p_->image) {
        p_->image->data = nullptr;
        XDestroyImage(p_->image);
        p_->image = nullptr;
    }
    XFlush(p_->dpy);
}

void X11Window::set_click_through(bool on) {
    if (!ok() || !have_shape_ || click_through_ == on) return;
    click_through_ = on;
    if (on) {
        // Порожня ВХІДНА область: вікно видно, але миша його не бачить.
        // Видима форма при цьому не міняється — саме тому область саме вхідна.
        Region empty = XCreateRegion();
        XShapeCombineRegion(p_->dpy, p_->win, ShapeInput, 0, 0, empty, ShapeSet);
        XDestroyRegion(empty);
    } else {
        XShapeCombineMask(p_->dpy, p_->win, ShapeInput, 0, 0, None, ShapeSet);
    }
    XFlush(p_->dpy);
}

void X11Window::present(const uint8_t* bgra, int w, int h) {
    if (!ok() || !bgra || w <= 0 || h <= 0) return;

    // XImage робимо обгорткою над чужим буфером: копії немає, X читає прямо
    // з нього. Пікселі в нас premultiplied BGRA, тобто в 32-бітному слові
    // little-endian це 0xAARRGGBB — рівно те, чого чекає ARGB-візуал.
    if (!p_->image || p_->image->width != w || p_->image->height != h) {
        if (p_->image) {
            p_->image->data = nullptr;
            XDestroyImage(p_->image);
        }
        p_->image = XCreateImage(p_->dpy, p_->visual, p_->depth, ZPixmap, 0,
                              (char*)bgra, w, h, 32, w * 4);
        if (!p_->image) return;
    }
    p_->image->data = (char*)bgra;
    XPutImage(p_->dpy, p_->win, p_->gc, p_->image, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
    XFlush(p_->dpy);
}

void X11Window::present_blank() {
    if (!ok()) return;
    XClearWindow(p_->dpy, p_->win);
    XFlush(p_->dpy);
}

void X11Window::start_drag(int mx, int my) {
    dragging_ = true;
    resizing_ = false;
    drag_dx_ = mx;
    drag_dy_ = my;
}

void X11Window::start_resize(int mx, int my) {
    resizing_ = true;
    dragging_ = false;
    drag_dx_ = mx;
    drag_dy_ = my;
    resize_w_ = w_;
    resize_h_ = h_;
}

void X11Window::end_drag() {
    dragging_ = false;
    resizing_ = false;
}

bool X11Window::poll_event(X11Event* out) {
    if (!ok() || !out || !XPending(p_->dpy)) return false;
    *out = X11Event();

    XEvent e;
    XNextEvent(p_->dpy, &e);
    switch (e.type) {
    case ButtonPress:
        if (e.xbutton.button == Button1) {
            out->press = true;
            out->mx = e.xbutton.x;
            out->my = e.xbutton.y;
        }
        break;
    case ButtonRelease:
        if (e.xbutton.button == Button1) {
            out->release = true;
            out->mx = e.xbutton.x;
            out->my = e.xbutton.y;
            end_drag();
        }
        break;
    case EnterNotify:
        out->motion = true;
        out->mx = e.xcrossing.x;
        out->my = e.xcrossing.y;
        break;
    case LeaveNotify:
        out->leave = true;
        break;
    case MotionNotify:
        out->motion = true;
        out->mx = e.xmotion.x;
        out->my = e.xmotion.y;
        if (dragging_) {
            // Тягнемо за смужку: курсор має лишатися в тій самій точці вікна,
            // інакше воно «стрибне» під нього першим же рухом.
            const int nx = e.xmotion.x_root - drag_dx_;
            const int ny = e.xmotion.y_root - drag_dy_;
            if (nx != x_ || ny != y_) {
                x_ = nx;
                y_ = ny;
                XMoveWindow(p_->dpy, p_->win, x_, y_);
                out->moved = true;
            }
        } else if (resizing_) {
            const int nw = resize_w_ + (e.xmotion.x - drag_dx_);
            const int nh = resize_h_ + (e.xmotion.y - drag_dy_);
            const int cw = nw < 160 ? 160 : nw;
            const int ch = nh < 120 ? 120 : nh;
            if (cw != w_ || ch != h_) {
                set_geometry(x_, y_, cw, ch);
                out->moved = true;
            }
        }
        break;
    case ConfigureNotify:
        if (e.xconfigure.width != w_ || e.xconfigure.height != h_) {
            w_ = e.xconfigure.width;
            h_ = e.xconfigure.height;
            out->moved = true;
        }
        break;
    case ClientMessage:
        if ((Atom)e.xclient.data.l[0] == p_->wm_delete) out->closed = true;
        break;
    default:
        break;
    }
    return true;
}

}  // namespace hominka
