#!/usr/bin/env python3
"""Minimal X input helper for interactive game testing.

Needs no root and no xdotool: it drives the X server's XTEST extension through
libX11 + libXtst via ctypes.

  xinput.py move X Y
  xinput.py drag X1 Y1 X2 Y2   (press, move in steps, release)
  xinput.py click X Y [button]        # button 1=left (default), 3=right
  xinput.py key <keysym>              # e.g. F10, Return, space, Escape
  xinput.py type "some text"
  xinput.py combo ctrl+shift+f7       # modifiers + key
  xinput.py pos                       # print the pointer position (XQueryPointer)
"""
import ctypes
import ctypes.util
import sys
import time

_x11 = ctypes.CDLL(ctypes.util.find_library("X11") or "libX11.so.6")
_xtst = ctypes.CDLL(ctypes.util.find_library("Xtst") or "libXtst.so.6")

_x11.XOpenDisplay.restype = ctypes.c_void_p
_x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
_x11.XStringToKeysym.restype = ctypes.c_ulong
_x11.XStringToKeysym.argtypes = [ctypes.c_char_p]
_x11.XKeysymToKeycode.restype = ctypes.c_ubyte
_x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
_x11.XFlush.argtypes = [ctypes.c_void_p]
_x11.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]

_xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
_xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
_xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]

# XQueryPointer for `pos`
class XQueryPointerArgs(ctypes.Structure):
    _fields_ = [("root", ctypes.c_ulong), ("child", ctypes.c_ulong),
                ("root_x", ctypes.c_int), ("root_y", ctypes.c_int),
                ("win_x", ctypes.c_int), ("win_y", ctypes.c_int),
                ("mask", ctypes.c_uint)]
_x11.XDefaultRootWindow.restype = ctypes.c_ulong
_x11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
_x11.XQueryPointer.restype = ctypes.c_int
_x11.XQueryPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                               ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int),
                               ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                               ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_uint)]

_dpy = _x11.XOpenDisplay(None)
if not _dpy:
    sys.exit("xinput: cannot open display")
_screen = 0

# window/geometry helpers
_x11.XQueryTree.restype = ctypes.c_int
_x11.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                            ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
                            ctypes.POINTER(ctypes.c_uint)]
_x11.XFetchName.restype = ctypes.c_int
_x11.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_char_p)]
_x11.XGetGeometry.restype = ctypes.c_int
_x11.XGetGeometry.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong),
                              ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
                              ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint),
                              ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
_x11.XSetInputFocus.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
_x11.XRaiseWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]


def _root():
    return _x11.XDefaultRootWindow(_dpy)


def _children(win):
    rr = ctypes.c_ulong(); pr = ctypes.c_ulong()
    kids = ctypes.POINTER(ctypes.c_ulong)()
    n = ctypes.c_uint()
    if not _x11.XQueryTree(_dpy, win, ctypes.byref(rr), ctypes.byref(pr), ctypes.byref(kids), ctypes.byref(n)):
        return []
    return [kids[i] for i in range(n.value)]


def _name(win):
    nm = ctypes.c_char_p()
    if _x11.XFetchName(_dpy, win, ctypes.byref(nm)) and nm.value:
        return nm.value.decode("latin-1")
    return ""


def _geom(win):
    root = ctypes.c_ulong(); x = ctypes.c_int(); y = ctypes.c_int()
    w = ctypes.c_uint(); h = ctypes.c_uint(); bw = ctypes.c_uint(); d = ctypes.c_uint()
    if not _x11.XGetGeometry(_dpy, win, ctypes.byref(root), ctypes.byref(x), ctypes.byref(y),
                             ctypes.byref(w), ctypes.byref(h), ctypes.byref(bw), ctypes.byref(d)):
        return None
    return (x.value, y.value, w.value, h.value)


def windows():
    for win in _children(_root()):
        frame = _geom(win)
        name = _name(win)
        if frame:
            print("0x%08x %4d,%4d %5dx%-5d %s" % (win, frame[0], frame[1], frame[2], frame[3], name))
        else:
            print("0x%08x (geom unavailable) %s" % (win, name))


def _window_under_pointer():
    rr = ctypes.c_ulong(); cr = ctypes.c_ulong()
    rx = ctypes.c_int(); ry = ctypes.c_int(); wx = ctypes.c_int(); wy = ctypes.c_int()
    mask = ctypes.c_uint()
    _x11.XQueryPointer(_dpy, _root(), ctypes.byref(rr), ctypes.byref(cr),
                       ctypes.byref(rx), ctypes.byref(ry), ctypes.byref(wx), ctypes.byref(wy),
                       ctypes.byref(mask))
    return cr.value


def focus():
    win = _window_under_pointer()
    if not win:
        return
    _x11.XSetInputFocus(_dpy, win, 2, 0)  # RevertToParent, CurrentTime
    _x11.XRaiseWindow(_dpy, win)
    _x11.XFlush(_dpy)
    time.sleep(0.1)



def _flush(delay_ms=30):
    _x11.XFlush(_dpy)
    time.sleep(delay_ms / 1000.0)


def move(x, y):
    _xtst.XTestFakeMotionEvent(_dpy, _screen, int(x), int(y), 0)
    _flush()


def drag(x1, y1, x2, y2, steps=12, button=1):
    """Press at (x1,y1), move in steps to (x2,y2), release."""
    move(x1, y1)
    _flush()
    _xtst.XTestFakeButtonEvent(_dpy, int(button), 1, 0)
    _flush()
    for i in range(1, steps + 1):
        x = x1 + (x2 - x1) * i // steps
        y = y1 + (y2 - y1) * i // steps
        move(x, y)
        _flush(10)
    _xtst.XTestFakeButtonEvent(_dpy, int(button), 0, 0)
    _flush()


def click(x, y, button=1):
    move(x, y)
    _xtst.XTestFakeButtonEvent(_dpy, button, 1, 0)
    _flush(20)
    _xtst.XTestFakeButtonEvent(_dpy, button, 0, 0)
    _flush()


def _keycode(keysym_name):
    ks = _x11.XStringToKeysym(keysym_name.encode())
    if ks == 0:
        sys.exit("xinput: unknown keysym %r" % keysym_name)
    kc = _x11.XKeysymToKeycode(_dpy, ks)
    if kc == 0:
        sys.exit("xinput: no keycode for %r" % keysym_name)
    return kc


def key(keysym_name):
    kc = _keycode(keysym_name)
    _xtst.XTestFakeKeyEvent(_dpy, kc, 1, 0)
    _flush(20)
    _xtst.XTestFakeKeyEvent(_dpy, kc, 0, 0)
    _flush()


def keyhold(keysym_name, ms=150):
    """Press and hold a key for ``ms`` milliseconds (for game hotkeys)."""
    kc = _keycode(keysym_name)
    _xtst.XTestFakeKeyEvent(_dpy, kc, 1, 0)
    _flush(ms)
    _xtst.XTestFakeKeyEvent(_dpy, kc, 0, 0)
    _flush()


def combo(spec):
    parts = spec.split("+")
    mods = [_keycode(p) for p in parts[:-1]]
    kc = _keycode(parts[-1])
    for m in mods:
        _xtst.XTestFakeKeyEvent(_dpy, m, 1, 0)
    _flush(20)
    _xtst.XTestFakeKeyEvent(_dpy, kc, 1, 0)
    _flush(20)
    _xtst.XTestFakeKeyEvent(_dpy, kc, 0, 0)
    _flush(20)
    for m in reversed(mods):
        _xtst.XTestFakeKeyEvent(_dpy, m, 0, 0)
    _flush()


def type_text(text):
    for ch in text:
        name = ch
        if ch == " ":
            name = "space"
        key(name)


def pos():
    root = _x11.XDefaultRootWindow(_dpy)
    rr = ctypes.c_ulong(); cr = ctypes.c_ulong()
    rx = ctypes.c_int(); ry = ctypes.c_int(); wx = ctypes.c_int(); wy = ctypes.c_int()
    mask = ctypes.c_uint()
    _x11.XQueryPointer(_dpy, root, ctypes.byref(rr), ctypes.byref(cr),
                       ctypes.byref(rx), ctypes.byref(ry),
                       ctypes.byref(wx), ctypes.byref(wy), ctypes.byref(mask))
    print(rx.value, ry.value)


def main(argv):
    if len(argv) >= 6 and argv[1] == "drag":
        drag(int(argv[2]), int(argv[3]), int(argv[4]), int(argv[5]))
        return 0
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd = argv[1]
    if cmd == "move":
        move(int(argv[2]), int(argv[3]))
    elif cmd == "click":
        click(int(argv[2]), int(argv[3]), int(argv[4]) if len(argv) > 4 else 1)
    elif cmd == "key":
        key(argv[2])
    elif cmd == "keyhold":
        keyhold(argv[2], int(argv[3]) if len(argv) > 3 else 150)
    elif cmd == "type":
        type_text(argv[2])
    elif cmd == "combo":
        combo(argv[2])
    elif cmd == "pos":
        pos()
    elif cmd == "windows":
        windows()
    elif cmd == "focus":
        focus()
    elif cmd == "clickf":
        focus()
        click(int(argv[2]), int(argv[3]), int(argv[4]) if len(argv) > 4 else 1)
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
