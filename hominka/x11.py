"""Linux: попросити композитор не зникати, коли гра йде на весь екран.

У Windows вікно ховають від захоплення і тримають зверху засобами системи. У
Linux засобів рівно два, і обидва — прохання до композитора:

  _NET_WM_BYPASS_COMPOSITOR = 2   «навіть якщо піді мною повноекранне вікно,
                                   продовжуй малювати мене» (значення 1 — це
                                   протилежне прохання, 0 — байдуже);
  _NET_WM_STATE_ABOVE             «тримай мене вище за інші вікна».

Працює це лише на X11 і лише якщо композитор слухає такі прохання (KWin, Picom,
Mutter — слухають). У Wayland такого немає взагалі: там поверх повноекранного
вікна малює лише шар `overlay` протоколу wlr-layer-shell, а це окрема
бібліотека на C++, якої в PySide6 немає. Тому для Wayland ми чесно кажемо
людині, що оверлей буде сховано, і радимо gamescope або гру у вікні.

Робимо все через ctypes і libX11 — жодної нової залежності. XID вікна дає сам
Qt (winId()).
"""

import ctypes
import os


def session_type() -> str:
    """x11 / wayland / порожньо, якщо не Linux або не зрозуміло."""
    if os.name != "posix":
        return ""
    session = (os.environ.get("XDG_SESSION_TYPE") or "").lower()
    if session in ("x11", "wayland"):
        return session
    if os.environ.get("WAYLAND_DISPLAY"):
        return "wayland"
    if os.environ.get("DISPLAY"):
        return "x11"
    return ""


def apply_overlay_hints(win) -> bool:
    """Ставить вікну обидві підказки. Повертає True, якщо вдалося.

    Мовчазна відмова тут доречна: підказки — покращення, а не умова роботи, і
    падати через відсутню libX11 вікно чату не повинно.
    """
    if session_type() != "x11":
        return False
    try:
        xlib = ctypes.cdll.LoadLibrary("libX11.so.6")
    except OSError:
        return False

    xlib.XOpenDisplay.restype = ctypes.c_void_p
    xlib.XInternAtom.restype = ctypes.c_ulong
    display = xlib.XOpenDisplay(None)
    if not display:
        return False
    try:
        window = ctypes.c_ulong(int(win.winId()))

        def atom(name: str) -> ctypes.c_ulong:
            return ctypes.c_ulong(xlib.XInternAtom(ctypes.c_void_p(display),
                                                   name.encode(), False))

        # _NET_WM_BYPASS_COMPOSITOR = 2 (не вимикати композитинг через мене)
        value = ctypes.c_ulong(2)
        xlib.XChangeProperty(ctypes.c_void_p(display), window,
                             atom("_NET_WM_BYPASS_COMPOSITOR"),
                             ctypes.c_ulong(6),      # XA_CARDINAL
                             32, 0,                  # PropModeReplace
                             ctypes.byref(value), 1)

        # _NET_WM_STATE_ABOVE — прохання тримати вище за інші вікна.
        above = atom("_NET_WM_STATE_ABOVE")
        xlib.XChangeProperty(ctypes.c_void_p(display), window,
                             atom("_NET_WM_STATE"),
                             ctypes.c_ulong(4),      # XA_ATOM
                             32, 1,                  # PropModeAppend
                             ctypes.byref(above), 1)
        xlib.XFlush(ctypes.c_void_p(display))
        return True
    except Exception:
        return False
    finally:
        xlib.XCloseDisplay(ctypes.c_void_p(display))


def advice() -> str:
    """Що сказати людині про її сесію (порожньо — казати нема чого)."""
    session = session_type()
    if session == "wayland":
        return ("Wayland: поверх повноекранної гри вікно підняти неможливо — "
                "такого не дозволяє сам протокол. Запускайте гру у вікні або "
                "через gamescope.")
    if session == "x11":
        return ("X11: попросили композитор не зникати під повноекранною грою. "
                "Якщо оверлея все одно не видно — знімайте гру не на весь екран, "
                "а вікном (Xcomposite / PipeWire) або через obs-vkcapture.")
    return ""
