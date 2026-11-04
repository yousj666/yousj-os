#!/usr/bin/env python3
"""Framebuffer/desktop acceptance: boot the UEFI ESP image and
verify the screen mechanically, milestone by milestone --
M81 the GOP framebuffer, M82/M83 the (now serial-side) console
and PS/2 keyboard, M84 the mouse arrow, M85/M86 windows and
the taskbar/start menu, M88 the graphical terminal, M89 the
menu's Terminal entry, and M91 the real desktop: wallpaper,
icons, select/double-click launch, the About box.

The monitor socket approach keeps the guest display headless
(-display none) while still owning a VGA device to dump.
"""
import os
import socket
import struct
import subprocess
import sys
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OVMF = os.path.expanduser(
    "~/workspace/toolchain/root/usr/share/OVMF/OVMF_CODE_4M.fd")
VARS_SRC = os.path.expanduser(
    "~/workspace/toolchain/root/usr/share/OVMF/OVMF_VARS_4M.fd")
ESP = os.path.join(ROOT, "build", "esp81", "esp.img")
VARS = os.path.join(ROOT, "build", "fb_vars.fd")
SER = "/tmp/serFB"
MON = "/tmp/monFB"
PPM = os.path.join(ROOT, "build", "fb_shot.ppm")
PNG = os.path.join(ROOT, "build", "fb_shot.png")


class Serial:
    def __init__(self, path):
        self.buf = b""
        self.path = path
        for _ in range(200):
            if os.path.exists(path):
                break
            time.sleep(0.05)
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(path)
        self.s.settimeout(0.2)

    def until(self, needle, timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            if needle.encode() in self.buf:
                return True
            try:
                d = self.s.recv(65536)
                if d:
                    self.buf += d
            except socket.timeout:
                pass
        return needle.encode() in self.buf

    def text(self):
        return self.buf.decode(errors="replace")

    def send(self, s):
        self.s.sendall(s.encode())


def read_ppm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    parts = data.split(b"\n", 3)
    magic = parts[0].strip()
    assert magic == b"P6", magic
    dims = parts[1].split()
    if len(dims) < 2:
        raise ValueError("unexpected PPM header")
    w, h = int(dims[0]), int(dims[1])
    maxv = int(parts[2])
    assert maxv == 255
    body = parts[3]
    return w, h, body


def save_png(path, w, h, rgb):
    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        c += struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        return c
    raw = b""
    stride = w * 3
    for y in range(h):
        raw += b"\x00" + rgb[y * stride:(y + 1) * stride]
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", ihdr)
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)


def px(body, w, x, y):
    i = (y * w + x) * 3
    return tuple(body[i:i + 3])


def close(a, b, tol=24):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


fails = []


def check(name, ok):
    print(("PASS " if ok else "FAIL ") + name)
    if not ok:
        fails.append(name)


def main():
    for p in (SER, MON):
        if os.path.exists(p):
            os.unlink(p)
    subprocess.run(["cp", VARS_SRC, VARS], check=True)
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-machine", "q35", "-m", "256",
        "-drive", "if=pflash,format=raw,readonly=on,file=" + OVMF,
        "-drive", "if=pflash,format=raw,file=" + VARS,
        "-drive", "format=raw,file=" + ESP + ",if=ide,index=2",
        "-serial", "unix:" + SER + ",server,nowait",
        "-monitor", "unix:" + MON + ",server,nowait",
        "-display", "none", "-no-reboot",
    ])
    try:
        ser = Serial(SER)
        check("guest boots to the shell (UEFI + GOP build)",
              ser.until("sh> ", 90))
        time.sleep(1.0)

        def dump(name):
            # The monitor can answer before the PPM is fully
            # written; on a loaded host a fixed sleep read STALE
            # frames (an arrow still mid-journey), which poisoned
            # every position decision downstream. Wait for the
            # file's mtime to actually advance.
            try:
                before = os.stat(name).st_mtime_ns
            except OSError:
                before = 0
            mon2 = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            mon2.connect(MON)
            mon2.settimeout(5)
            mon2.recv(4096)
            mon2.sendall(("screendump " + name + "\n").encode())
            time.sleep(0.4)
            try:
                mon2.recv(4096)
            except socket.timeout:
                pass
            mon2.close()
            for _ in range(40):
                try:
                    if os.stat(name).st_mtime_ns > before:
                        break
                except OSError:
                    pass
                time.sleep(0.1)
            return read_ppm(name)

        def mon_cmd(cmd, wait=0.3):
            m = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            m.connect(MON)
            m.settimeout(5)
            m.recv(4096)
            m.sendall((cmd + "\n").encode())
            time.sleep(wait)
            try:
                m.recv(4096)
            except socket.timeout:
                pass
            m.close()

        def sendkey(key):
            m = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            m.connect(MON)
            m.settimeout(5)
            m.recv(4096)
            m.sendall(("sendkey " + key + "\n").encode())
            time.sleep(0.22)
            m.close()

        def click():
            mon_cmd("mouse_button 1", 0.5)
            mon_cmd("mouse_button 0", 0.4)

        def dblclick():
            # press-to-press well inside the kernel's 60-tick
            # double-click window
            mon_cmd("mouse_button 1", 0.12)
            mon_cmd("mouse_button 0", 0.12)
            mon_cmd("mouse_button 1", 0.12)
            mon_cmd("mouse_button 0", 0.3)

        w, h, body = dump(PPM)
        log = ser.text()
        check("kernel reports the framebuffer on serial",
              "Yousj OS: fb " in log)
        check("no exceptions", "EXCEPTION" not in log)
        print("screendump: %dx%d" % (w, h))
        check("resolution at least 800x600", w >= 800 and h >= 600)

        def wall(y):
            # the kernel's gradient, recomputed
            t = (y * 255) // h
            return (0x17 - (0x0D * t) // 255,
                    0x35 - (0x1B * t) // 255,
                    0x50 - (0x24 * t) // 255)

        # --- M91: the desktop is the screen -------------------
        check("wallpaper gradient mid-screen",
              close(px(body, w, 1024, 700), wall(700), 6))
        check("wallpaper gradient lower",
              close(px(body, w, 1024, 1500), wall(1500), 6))
        check("old swatch spot is wallpaper now",
              close(px(body, w, int(w * 1.5 / 8), h - 90),
                    wall(h - 90), 6))
        # no console text wall: right of the icon column the
        # desktop is pure wallpaper
        bright = 0
        for yy in range(150, 1900, 3):
            for xx in range(220, 2040, 3):
                r, g, b = px(body, w, xx, yy)
                if r > 150 and g > 170 and b > 200:
                    bright += 1
        print("stray bright samples on the desktop:", bright)
        check("no console text wall on the desktop", bright < 5)
        check("Terminal icon tile",
              close(px(body, w, 100, 34), (0x0A, 0x14, 0x1F)))
        check("About icon tile",
              close(px(body, w, 60, 158), (0x2E, 0x5E, 0x8C)))
        lbl = 0
        for yy in range(104, 128):
            for xx in range(24, 122):
                r, g, b = px(body, w, xx, yy)
                if r > 200 and g > 200 and b > 200:
                    lbl += 1
        print("Terminal label bright px:", lbl)
        check("icon labels painted", lbl > 20)
        check("taskbar present",
              close(px(body, w, 600, 2028), (0x1B, 0x3A, 0x5C)))
        check("start button present",
              close(px(body, w, 30, 2028), (0x2E, 0x5E, 0x8C)))
        check("desktop starts clean: no window chip",
              close(px(body, w, 150, 2028), (0x1B, 0x3A, 0x5C)))
        save_png(PNG, w, h, body)
        print("screenshot saved:", PNG)

        # --- M83: the keyboard still drives the serial shell --
        for key in ("e", "c", "h", "o", "spc", "shift-k",
                    "shift-b", "shift-d", "8", "3"):
            sendkey(key)
        sendkey("ret")
        check("keyboard-typed command ran (KBD83 on serial)",
              ser.until("KBD83", 30))

        # --- M84: the arrow follows, clicks leave no mark -----
        ARROW = [
            "#...........", "##..........", "#o#.........",
            "#oo#........", "#ooo#.......", "#oooo#......",
            "#ooooo#.....", "#oooooo#....", "#ooooooo#...",
            "#oooooooo#..", "#oooooo#####", "#oo#oo#.....",
            "#o#.#oo#....", "##..#oo#....", "#....#oo#...",
            "......#oo#..", ".......##...",
        ]

        def find_arrow(body2):
            import numpy as np
            arr = np.frombuffer(body2, dtype=np.uint8).reshape(
                h, w, 3).astype(np.int16)
            black = (arr[:, :, 0] < 30) & (arr[:, :, 1] < 30) & \
                (arr[:, :, 2] < 30)
            white = (arr[:, :, 0] > 240) & (arr[:, :, 1] > 240) & \
                (arr[:, :, 2] > 240)
            rows, cols = h - 16, w - 11
            score = np.zeros((rows, cols), dtype=np.int32)
            need = 0
            for r, row in enumerate(ARROW):
                for c, ch in enumerate(row):
                    if ch == '#':
                        score += black[r:r + rows, c:c + cols]
                        need += 1
                    elif ch == 'o':
                        score += white[r:r + rows, c:c + cols]
                        need += 1
            ys, xs = np.nonzero(score == need)
            if len(xs) == 0:
                return None
            return (int(xs[0]), int(ys[0]))

        def goto(ex, ey, frm):
            # One open-loop trip, then verify. QEMU accumulates
            # a large PS/2 move internally and each following
            # mouse event flushes only a bounded chunk (~635px),
            # so the trip ends with zero-move flushes; a second
            # corrective trip would stack on the undelivered
            # remainder and overshoot into a corner.
            def trip(tx, ty, fx, fy):
                mon_cmd("mouse_move %d %d" % (tx - fx, ty - fy),
                        0.3)
                dist = max(abs(tx - fx), abs(ty - fy))
                for _ in range(2 + dist // 400):
                    mon_cmd("mouse_move 0 0", 0.35)
                time.sleep(1.5)

            trip(ex, ey, frm[0], frm[1])
            _, _, b2 = dump("/tmp/settle.ppm")
            pos = find_arrow(b2)
            print("goto (%d,%d) -> arrow %s" % (ex, ey, pos))
            return pos == (ex, ey)

        def white_at(body2, cx, cy, rad=14):
            n = 0
            for yy in range(max(0, cy - rad), min(h, cy + rad)):
                for xx in range(max(0, cx - rad), min(w, cx + rad)):
                    r, g, b = px(body2, w, xx, yy)
                    if r > 240 and g > 240 and b > 240:
                        n += 1
            return n

        w, h, body = dump(os.path.join(ROOT, "build",
                                       "fb_m0.ppm"))
        centre = white_at(body, w // 2, h // 2)
        print("cursor white px at centre:", centre)
        check("arrow sits at screen centre", centre > 10)
        goto(1224, 1124, (1024, 1024))
        w, h, body = dump(os.path.join(ROOT, "build",
                                       "fb_m1.ppm"))
        moved = white_at(body, w // 2 + 200, h // 2 + 100)
        gone = white_at(body, w // 2, h // 2)
        print("cursor white px at +200,+100:", moved,
              "centre now:", gone)
        check("arrow followed the mouse", moved > 10)
        check("arrow vacated the centre (save-under)",
              gone < 5)
        goto(1500, 1500, (1224, 1124))
        click()
        w, h, body = dump(os.path.join(ROOT, "build",
                                       "fb_m2.ppm"))
        check("bare desktop click leaves no mark (M91)",
              close(px(body, w, 1514, 1514), wall(1514), 6))
        save_png(os.path.join(ROOT, "build", "fb_mouse.png"),
                 w, h, body)

        # --- M91: icon select, deselect, double-click launch --
        def sel_band():
            n = 0
            for yy in range(106, 126):
                for xx in range(24, 120):
                    r, g, b = px(body, w, xx, yy)
                    if b > 112 and r < 0x40:
                        n += 1
            return n

        goto(72, 60, (1500, 1500))
        click()
        w, h, body = dump(os.path.join(ROOT, "build",
                                       "fb_i1.ppm"))
        sel = sel_band()
        print("selection band px after one click:", sel)
        check("single click selects the Terminal icon",
              sel > 800)
        goto(1500, 1500, (72, 60))
        click()
        w, h, body = dump(os.path.join(ROOT, "build",
                                       "fb_i2.ppm"))
        desel = sel_band()
        print("selection band px after desktop click:", desel)
        check("desktop click clears the selection", desel < 50)
        goto(72, 60, (1500, 1500))
        dblclick()
        time.sleep(2.5)
        w, h, body = dump(os.path.join(ROOT, "build",
                                       "fb_t1.ppm"))
        check("double-click opened the terminal window",
              close(px(body, w, 500, 310), (0x2E, 0x5E, 0x8C)))
        check("terminal window body",
              close(px(body, w, 700, 700), (0x24, 0x46, 0x6B)))

        def win_text_pixels():
            n = 0
            for yy in range(336, 856, 2):
                for xx in range(308, 1172, 2):
                    r_, g_, b_ = px(body, w, xx, yy)
                    if r_ > 200 and g_ > 200 and b_ > 200:
                        n += 1
            return n

        before = win_text_pixels()
        check("shell banner rendered in the window",
              before > 60)
        for key in ("e", "c", "h", "o", "spc", "shift-g",
                    "shift-u", "shift-i", "9", "1"):
            sendkey(key)
        sendkey("ret")
        time.sleep(2.0)
        w, h, body = dump(os.path.join(ROOT, "build",
                                       "fb_t2.ppm"))
        after = win_text_pixels()
        print("window text pixels before/after echo:",
              before, after)
        check("typed command ran inside the window (GUI91)",
              after > before + 15)
        save_png(os.path.join(ROOT, "build", "fb_term.png"),
                 w, h, body)
        goto(1166, 310, (72, 60))
        click()
        time.sleep(1.5)
        w, h, body = dump(os.path.join(ROOT, "build",
                                       "fb_t3.ppm"))
        check("terminal closed, wallpaper restored",
              close(px(body, w, 500, 310), wall(310), 6))
        check("terminal body gone",
              not close(px(body, w, 1100, 800),
                        (0x24, 0x46, 0x6B)))
        for key in ("e", "c", "h", "o", "spc", "shift-b",
                    "shift-a", "shift-c", "shift-k", "9", "1"):
            sendkey(key)
        sendkey("ret")
        back_ok = ser.until("BACK91", 30)
        check("console shell live behind the desktop (BACK91)",
              back_ok)
        if not back_ok:
            print("serial tail:", repr(ser.text()[-600:]))

        # --- M91: the About icon, its window, a drag ----------
        goto(72, 184, (1166, 310))
        dblclick()
        time.sleep(1.2)
        w, h, body = dump(os.path.join(ROOT, "build",
                                       "fb_a1.ppm"))
        check("About window opened centered (title)",
              close(px(body, w, 754, 860), (0x2E, 0x5E, 0x8C)))
        check("About window body",
              close(px(body, w, 1000, 1000),
                    (0x24, 0x46, 0x6B)))
        check("taskbar chip names the About window",
              close(px(body, w, 150, 2028), (0x24, 0x46, 0x6B)))
        save_png(os.path.join(ROOT, "build", "fb_about.png"),
                 w, h, body)
        goto(1000, 868, (72, 184))
        mon_cmd("mouse_button 1", 0.5)
        goto(1150, 988, (1000, 868))
        mon_cmd("mouse_button 0", 0.4)
        w, h, body = dump(os.path.join(ROOT, "build",
                                       "fb_a2.ppm"))
        check("About window followed the drag",
              close(px(body, w, 1280, 1154),
                    (0x24, 0x46, 0x6B)))
        check("drag start repainted to wallpaper",
              close(px(body, w, 760, 880), wall(880), 6))
        goto(1440, 984, (1150, 988))
        click()
        w, h, body = dump(os.path.join(ROOT, "build",
                                       "fb_a3.ppm"))
        check("About closed, wallpaper restored",
              close(px(body, w, 904, 980), wall(980), 6))

        # --- M86/M89: the start menu still launches -----------
        goto(56, 2028, (1440, 984))
        click()
        w, h, body = dump(os.path.join(ROOT, "build",
                                       "fb_m91a.ppm"))
        check("start menu opened (two entries)",
              close(px(body, w, 200, 2000), (0x24, 0x46, 0x6B)))
        goto(100, 1988, (56, 2028))
        click()
        time.sleep(2.5)
        w, h, body = dump(os.path.join(ROOT, "build",
                                       "fb_m91b.ppm"))
        check("menu Terminal entry opened a terminal window",
              close(px(body, w, 500, 310), (0x2E, 0x5E, 0x8C)))
        check("menu area repainted to wallpaper",
              close(px(body, w, 200, 2000), wall(2000), 6))
        check("taskbar chip names the menu-launched terminal",
              close(px(body, w, 150, 2028), (0x24, 0x46, 0x6B)))
        save_png(os.path.join(ROOT, "build", "fb_menu.png"),
                 w, h, body)
        goto(1166, 310, (100, 1988))
        click()
        time.sleep(1.5)
        w, h, body = dump(os.path.join(ROOT, "build",
                                       "fb_m91c.ppm"))
        check("menu-launched terminal closed by its red box",
              not close(px(body, w, 1100, 800),
                        (0x24, 0x46, 0x6B)))
        for key in ("e", "c", "h", "o", "spc", "shift-m",
                    "shift-e", "shift-n", "shift-u", "9", "1"):
            sendkey(key)
        sendkey("ret")
        menu_ok = ser.until("MENU91", 30)
        check("console shell live after menu launch (MENU91)",
              menu_ok)
        if not menu_ok:
            print("serial tail:", repr(ser.text()[-400:]))
    finally:
        qemu.terminate()
        try:
            qemu.wait(timeout=5)
        except subprocess.TimeoutExpired:
            qemu.kill()
    print("FAILURES PRESENT" if fails else "ALL OK")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
