#!/usr/bin/env python3
"""M81 acceptance: boot the UEFI ESP image, let the kernel light
the GOP framebuffer, take a QEMU screendump, and verify the
pixels mechanically (background, title bar, channel swatches),
plus the serial "Yousj OS: fb ..." line. Also saves a PNG next to
the PPM so a human can look at the first Yousj screen.

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
    # header: P6 <ws> W <ws> H <ws> MAX <one ws>
    parts = data.split(b"\n", 3)
    magic = parts[0].strip()
    assert magic == b"P6", magic
    dims = parts[1].split()
    if len(dims) < 2:  # comment line possible; not expected from QEMU
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
        mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        mon.connect(MON)
        mon.settimeout(5)
        mon.recv(4096)
        mon.sendall(("screendump " + PPM + "\n").encode())
        time.sleep(1.5)
        try:
            mon.recv(4096)
        except socket.timeout:
            pass
        mon.close()
        log = ser.text()
        check("kernel reports the framebuffer on serial",
              "Yousj OS: fb " in log)
        check("no exceptions", "EXCEPTION" not in log)
        w, h, body = read_ppm(PPM)
        print("screendump: %dx%d" % (w, h))
        check("resolution at least 800x600", w >= 800 and h >= 600)
        check("background is the banner field color",
              close(px(body, w, w // 4, h // 2), (0x10, 0x24, 0x3E)))
        check("title bar color",
              close(px(body, w, w // 2, 20), (0x1B, 0x3A, 0x5C)))
        swy = h - 90
        check("red swatch is red",
              close(px(body, w, int(w * 1.5 / 8), swy), (255, 0, 0)))
        check("green swatch is green",
              close(px(body, w, int(w * 3.5 / 8), swy), (0, 255, 0)))
        check("blue swatch is blue",
              close(px(body, w, int(w * 5.5 / 8), swy), (0, 0, 255)))
        # the title text must have painted bright pixels in the bar
        bright = 0
        for yy in range(8, 56, 2):
            for xx in range(16, 220, 2):
                r, g, b = px(body, w, xx, yy)
                if r > 200 and g > 200 and b > 200:
                    bright += 1
        check("title text pixels present", bright > 30)
        save_png(PNG, w, h, body)
        print("screenshot saved:", PNG)

        # --- M82: the framebuffer console mirrors serial output ---
        def band_bright(y0, y1):
            n = 0
            for yy in range(y0, min(y1, h), 2):
                for xx in range(8, min(1400, w), 2):
                    r, g, b = px(body, w, xx, yy)
                    if r > 150 and g > 170 and b > 200:
                        n += 1
            return n

        def dump(name):
            # The monitor can answer before the PPM is fully
            # written; on a loaded host the old fixed sleep read
            # STALE frames (an arrow still mid-journey), which
            # poisoned every position decision downstream. Wait
            # for the file's mtime to actually advance.
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

        w2, h2, body = dump(os.path.join(ROOT, "build", "fb_con1.ppm"))
        upper = band_bright(200, 560)
        lower_before = band_bright(560, 1000)
        print("console bright samples: upper", upper,
              "lower", lower_before)
        check("console shows mirrored boot text", upper > 400)
        ser.send("echo FBCON82\n")
        check("shell echoes the marker on serial",
              ser.until("FBCON82", 30))
        time.sleep(0.6)
        w2, h2, body = dump(os.path.join(ROOT, "build", "fb_con2.ppm"))
        lower_after = band_bright(560, 1000)
        print("console bright samples after echo: lower", lower_after)
        check("console advanced with new shell output",
              lower_after > lower_before + 20)
        save_png(os.path.join(ROOT, "build", "fb_con.png"), w, h, body)

        # --- M83: type on the PS/2 keyboard via monitor sendkey ---
        def sendkey(key):
            m = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            m.connect(MON)
            m.settimeout(5)
            m.recv(4096)
            m.sendall(("sendkey " + key + "\n").encode())
            time.sleep(0.22)
            m.close()

        for key in ("e", "c", "h", "o", "spc", "shift-k", "shift-b",
                    "shift-d", "8", "3"):
            sendkey(key)
        sendkey("ret")
        check("keyboard-typed command ran (KBD83 on serial)",
              ser.until("KBD83", 30))
        time.sleep(0.6)
        w2, h2, body = dump(os.path.join(ROOT, "build", "fb_kbd.ppm"))
        lower_kbd = band_bright(560, 1000)
        print("console bright samples after keyboard:", lower_kbd)
        check("console advanced with keyboard input",
              lower_kbd > lower_after + 20)
        save_png(os.path.join(ROOT, "build", "fb_kbd.png"), w, h, body)

        # --- M84: move the PS/2 mouse, watch the arrow follow ---
        ARROW = [
            "#...........", "##..........", "#o#.........",
            "#oo#........", "#ooo#.......", "#oooo#......",
            "#ooooo#.....", "#oooooo#....", "#ooooooo#...",
            "#oooooooo#..", "#oooooo#####", "#oo#oo#.....",
            "#o#.#oo#....", "##..#oo#....", "#....#oo#...",
            "......#oo#..", ".......##...",
        ]

        def find_arrow(body2):
            # numpy template match: a pure-Python scan of a
            # 2048x2048 dump takes seconds and starves the VM
            # it is measuring; this takes milliseconds.
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
            # Open-loop move + a long blind wait + one look.
            # QEMU delivers a large PS/2 move packet by packet
            # (+-127 each) over seconds, and screendump polling
            # itself stalls that delivery -- every closed-loop
            # scheme tried here (settle watching, stable-park
            # nudges) ended up double-delivering and overshoot-
            # ing into a corner. A single move, patience scaled
            # to the distance, and at most one correction from
            # the verified resting position is what works.
            def trip(tx, ty, fx, fy):
                mon_cmd("mouse_move %d %d" % (tx - fx, ty - fy),
                        0.3)
                dist = max(abs(tx - fx), abs(ty - fy))
                # QEMU accumulates a big move internally and
                # each FOLLOWING mouse event flushes only a
                # bounded chunk of it (~5 packets, 635px --
                # measured: a -1690 move showed 5 packets
                # delivered at verify time, 10 by the next
                # press, the rest only after another event).
                # Flush with harmless zero moves, then rest.
                for _ in range(2 + dist // 400):
                    mon_cmd("mouse_move 0 0", 0.35)
                time.sleep(1.5)

            # NO correction trip: every scheme that moved the
            # mouse a second time based on an observed position
            # overshot, because the observed frame lags the
            # still-travelling arrow and the "correction" then
            # stacks on top of the undelivered remainder. One
            # trip, then verify (the caller's pixel checks and
            # the kernel's own press log judge the result).
            trip(ex, ey, frm[0], frm[1])
            _, _, b2 = dump("/tmp/settle.ppm")
            pos = find_arrow(b2)
            print("goto (%d,%d) -> arrow %s" % (ex, ey, pos))
            return pos == (ex, ey)

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

        def white_at(body2, cx, cy, rad=14):
            n = 0
            for yy in range(max(0, cy - rad), min(h, cy + rad)):
                for xx in range(max(0, cx - rad), min(w, cx + rad)):
                    r, g, b = px(body2, w, xx, yy)
                    if r > 240 and g > 240 and b > 240:
                        n += 1
            return n

        w2, h2, body = dump(os.path.join(ROOT, "build", "fb_m0.ppm"))
        centre = white_at(body, w // 2, h // 2)
        print("cursor white px at centre:", centre)
        check("arrow sits at screen centre", centre > 10)
        goto(1224, 1124, (1024, 1024))
        w2, h2, body = dump(os.path.join(ROOT, "build", "fb_m1.ppm"))
        moved = white_at(body, w // 2 + 200, h // 2 + 100)
        gone = white_at(body, w // 2, h // 2)
        print("cursor white px at +200,+100:", moved,
              "centre now:", gone)
        check("arrow followed the mouse", moved > 10)
        check("arrow vacated the centre (save-under)", gone < 5)
        # click on the desktop (below the M85 window) for a marker.
        # NOTE: HMP mouse_move sends its arguments as RELATIVE
        # deltas for the PS/2 mouse (verified against dumps:
        # the applied motion equals the arguments, not a delta
        # from any tracked absolute position).
        goto(1500, 1500, (1224, 1124))
        mon_cmd("mouse_button 1", 0.5)
        mon_cmd("mouse_button 0", 0.3)
        w2, h2, body = dump(os.path.join(ROOT, "build", "fb_m2.ppm"))
        mark = 0
        for yy in range(1500, 1545):
            for xx in range(1500, 1545):
                r, g, b = px(body, w, xx, yy)
                if r > 240 and 190 < g < 240 and b < 140:
                    mark += 1
        print("click marker px:", mark)
        check("click dropped its marker", mark > 10)
        save_png(os.path.join(ROOT, "build", "fb_mouse.png"), w, h, body)

        # --- M85: the window exists, drags, and closes ---
        check("window body present",
              close(px(body, w, 1330, 1264), (0x24, 0x46, 0x6B)))
        check("window title bar present",
              close(px(body, w, 1330, 1094), (0x2E, 0x5E, 0x8C)))
        check("window close box present",
              close(px(body, w, 1589, 1087), (0xC0, 0x39, 0x2B)))
        # grab the title at (1200,1094): from (1500,1500)
        goto(1200, 1094, (1500, 1500))
        mon_cmd("mouse_button 1", 0.5)
        goto(1350, 1214, (1200, 1094))
        mon_cmd("mouse_button 0", 0.4)
        w2, h2, body = dump(os.path.join(ROOT, "build", "fb_w1.ppm"))
        check("window followed the drag",
              close(px(body, w, 1480, 1384), (0x24, 0x46, 0x6B)))
        check("window title moved too",
              close(px(body, w, 1480, 1214), (0x2E, 0x5E, 0x8C)))
        check("old window spot repainted to console",
              close(px(body, w, 1100, 1130), (0x10, 0x24, 0x3E)))
        save_png(os.path.join(ROOT, "build", "fb_win.png"), w, h, body)
        # close it: box centre now at (1746,1214): from (1350,1214)
        goto(1746, 1214, (1350, 1214))
        mon_cmd("mouse_button 1", 0.5)
        mon_cmd("mouse_button 0", 0.4)
        w2, h2, body = dump(os.path.join(ROOT, "build", "fb_w2.ppm"))
        check("window closed, console restored",
              close(px(body, w, 1480, 1384), (0x10, 0x24, 0x3E)))
        save_png(os.path.join(ROOT, "build", "fb_closed.png"), w, h,
                 body)

        # --- M86: taskbar, start menu, reopen the window ---
        # pointer is at (1746,1214); the window is closed now.
        check("taskbar present",
              close(px(body, w, 600, 2028), (0x1B, 0x3A, 0x5C)))
        check("start button present",
              close(px(body, w, 30, 2028), (0x2E, 0x5E, 0x8C)))
        check("no window chip while closed",
              close(px(body, w, 150, 2028), (0x1B, 0x3A, 0x5C)))
        goto(56, 2028, (1746, 1214))
        mon_cmd("mouse_button 1", 0.5)
        mon_cmd("mouse_button 0", 0.4)
        w2, h2, body = dump(os.path.join(ROOT, "build", "fb_d1.ppm"))
        check("start menu opened",
              close(px(body, w, 200, 2000), (0x24, 0x46, 0x6B)))
        goto(100, 1984, (56, 2028))
        mon_cmd("mouse_button 1", 0.5)
        mon_cmd("mouse_button 0", 0.4)
        w2, h2, body = dump(os.path.join(ROOT, "build", "fb_d2.ppm"))
        check("menu reopened the window",
              close(px(body, w, 1330, 1094), (0x2E, 0x5E, 0x8C)))
        check("menu closed itself",
              close(px(body, w, 200, 2000), (0x10, 0x24, 0x3E)))
        check("window chip back on the taskbar",
              close(px(body, w, 150, 2028), (0x24, 0x46, 0x6B)))

        save_png(os.path.join(ROOT, "build", "fb_desk.png"), w, h,
                 body)
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
