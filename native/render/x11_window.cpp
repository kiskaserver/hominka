#include "x11_window.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

#include <cstdio>
#include <cstring>

namespace hominka {

X11Window::~X11Window() { destroy(); }

bool X11Window::create(int x, int y, int w, int h, const char* title) {
    dpy_ = XOpenDisplay(nullptr);
    if (!dpy_) return false;
    screen_ = DefaultScreen(dpy_);
    title_ = title ? title : "Hominka";

    // 32-бітний ARGB-візуал: без нього прозорого тла не буде взагалі.
    XVisualInfo vi{};
    if (!XMatchVisualInfo(dpy_, screen_, 32, TrueColor, &vi)) {
        XCloseDisplay(dpy_);
        dpy_ = nullptr;
        return false;
    }
    visual_ = vi.visual;
    depth_ = vi.depth;

    cmap_ = XCreateColormap(dpy_, RootWindow(dpy_, screen_), visual_, AllocNone);

    XSetWindowAttributes attr{};
    attr.colormap = cmap_;
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
    win_ = XCreateWindow(dpy_, RootWindow(dpy_, screen_), x_, y_, w_, h_, 0,
                         depth_, InputOutput, visual_,
                         CWColormap | CWBackPixel | CWBorderPixel |
                         CWOverrideRedirect | CWEventMask, &attr);
    if (!win_) {
        XCloseDisplay(dpy_);
        dpy_ = nullptr;
        return false;
    }

    XStoreName(dpy_, win_, title_.c_str());
    wm_delete_ = XInternAtom(dpy_, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy_, win_, &wm_delete_, 1);
    apply_above();

    XGCValues gcv{};
    gc_ = XCreateGC(dpy_, win_, 0, &gcv);

    int shape_ev = 0, shape_err = 0;
    have_shape_ = XShapeQueryExtension(dpy_, &shape_ev, &shape_err) == True;

    XFlush(dpy_);
    return true;
}

void X11Window::apply_above() {
    // Навіть при override-redirect підказуємо композитору намір: деякі
    // середовища за цими властивостями вирішують, куди класти вікно.
    Atom above = XInternAtom(dpy_, "_NET_WM_STATE_ABOVE", False);
    Atom state = XInternAtom(dpy_, "_NET_WM_STATE", False);
    XChangeProperty(dpy_, win_, state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char*)&above, 1);

    Atom type = XInternAtom(dpy_, "_NET_WM_WINDOW_TYPE", False);
    Atom dock = XInternAtom(dpy_, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    XChangeProperty(dpy_, win_, type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char*)&dock, 1);
}

void X11Window::destroy() {
    if (!dpy_) return;
    if (image_) {
        // XImage лише обгортає наш буфер — щоб X його не звільняв.
        image_->data = nullptr;
        XDestroyImage(image_);
        image_ = nullptr;
    }
    if (gc_) { XFreeGC(dpy_, gc_); gc_ = nullptr; }
    if (win_) { XDestroyWindow(dpy_, win_); win_ = 0; }
    if (cmap_) { XFreeColormap(dpy_, cmap_); cmap_ = 0; }
    XCloseDisplay(dpy_);
    dpy_ = nullptr;
    shown_ = false;
}

void X11Window::show() {
    if (!ok() || shown_) return;
    XMapRaised(dpy_, win_);
    XFlush(dpy_);
    shown_ = true;
}

void X11Window::hide() {
    if (!ok() || !shown_) return;
    XUnmapWindow(dpy_, win_);
    XFlush(dpy_);
    shown_ = false;
}

void X11Window::set_geometry(int x, int y, int w, int h) {
    if (!ok()) return;
    if (w <= 0) w = w_;
    if (h <= 0) h = h_;
    if (x == x_ && y == y_ && w == w_ && h == h_) return;
    x_ = x; y_ = y; w_ = w; h_ = h;
    XMoveResizeWindow(dpy_, win_, x_, y_, (unsigned)w_, (unsigned)h_);
    // Розмір змінився — стара обгортка вже не про цей буфер.
    if (image_) {
        image_->data = nullptr;
        XDestroyImage(image_);
        image_ = nullptr;
    }
    XFlush(dpy_);
}

void X11Window::set_click_through(bool on) {
    if (!ok() || !have_shape_ || click_through_ == on) return;
    click_through_ = on;
    if (on) {
        // Порожня ВХІДНА область: вікно видно, але миша його не бачить.
        // Видима форма при цьому не міняється — саме тому область саме вхідна.
        Region empty = XCreateRegion();
        XShapeCombineRegion(dpy_, win_, ShapeInput, 0, 0, empty, ShapeSet);
        XDestroyRegion(empty);
    } else {
        XShapeCombineMask(dpy_, win_, ShapeInput, 0, 0, None, ShapeSet);
    }
    XFlush(dpy_);
}

void X11Window::present(const uint8_t* bgra, int w, int h) {
    if (!ok() || !bgra || w <= 0 || h <= 0) return;

    // XImage робимо обгорткою над чужим буфером: копії немає, X читає прямо
    // з нього. Пікселі в нас premultiplied BGRA, тобто в 32-бітному слові
    // little-endian це 0xAARRGGBB — рівно те, чого чекає ARGB-візуал.
    if (!image_ || image_->width != w || image_->height != h) {
        if (image_) {
            image_->data = nullptr;
            XDestroyImage(image_);
        }
        image_ = XCreateImage(dpy_, visual_, depth_, ZPixmap, 0,
                              (char*)bgra, w, h, 32, w * 4);
        if (!image_) return;
    }
    image_->data = (char*)bgra;
    XPutImage(dpy_, win_, gc_, image_, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
    XFlush(dpy_);
}

void X11Window::present_blank() {
    if (!ok()) return;
    XClearWindow(dpy_, win_);
    XFlush(dpy_);
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
    if (!ok() || !out || !XPending(dpy_)) return false;
    *out = X11Event();

    XEvent e;
    XNextEvent(dpy_, &e);
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
                XMoveWindow(dpy_, win_, x_, y_);
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
        if ((Atom)e.xclient.data.l[0] == wm_delete_) out->closed = true;
        break;
    default:
        break;
    }
    return true;
}

}  // namespace hominka
