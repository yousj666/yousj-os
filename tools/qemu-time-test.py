#!/usr/bin/env python3
"""M23 acceptance: boot and run timetest twice in the
shell (ramfs target /tt-file). Both runs must print all
six PASS lines and exit 0, and free-page counts must
agree (timetest leaves nothing behind)."""
import socket
import subprocess
import sys
import os
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK = "/tmp/serM23"
SCENARIOS = ["epoch", "time-ptr", "gettimeofday", "clocks",
             "clock-bad", "mtime"]
fails = []


class Session:
    def __init__(self):
        try:
            os.unlink(SOCK)
        except FileNotFoundError:
            pass
        self.qemu = subprocess.Popen(
            ["qemu-system-x86_64", "-m", "128",
             "-kernel", "build/yousj-stub.elf",
             "-initrd", "build/yousj-kernel.elf",
             "-display", "none",
             "-serial", "unix:" + SOCK + ",server,nowait",
             "-monitor", "none", "-no-reboot"],
            cwd=REPO, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
        for _ in range(50):
            if os.path.exists(SOCK):
                break
            time.sleep(0.2)
        self.sock = socket.socket(socket.AF_UNIX,
                                  socket.SOCK_STREAM)
        self.sock.connect(SOCK)
        self.sock.settimeout(0.5)
        self.buf = b""

    def until(self, needle, timeout=25.0):
        end = time.time() + timeout
        while needle.encode() not in self.buf:
            if time.time() > end:
                return False
            try:
                d = self.sock.recv(65536)
                if d:
                    self.buf += d
            except socket.timeout:
                pass
        return True

    def send(self, line, timeout=20.0):
        if not self.until("sh> ", timeout):
            return False
        self.buf = b""
        self.sock.send(line.encode() + b"\n")
        return True

    def run_expect(self, cmd, needle, timeout=25.0):
        if not self.send(cmd):
            return False
        return self.until(needle, timeout)

    def check(self, name, ok):
        print(("PASS " if ok else "FAIL ") + name)
        if not ok:
            fails.append(name)

    def close(self):
        self.qemu.terminate()


def free_count(sess):
    if not sess.run_expect("free", "sh> ", 25):
        return None
    if b"free pages:" not in sess.buf:
        return None
    tail = sess.buf.decode(errors="replace")
    for line in tail.splitlines():
        if "free pages:" in line:
            try:
                return int(line.split("free pages:")[1]
                           .split("of")[0].strip())
            except (ValueError, IndexError):
                return None
    return None


def main():
    sess = Session()
    counts = []
    try:
        sess.check("shell prompt", sess.until("sh> "))
        for run in (1, 2):
            sess.check(f"run{run} timetest done",
                       sess.run_expect("timetest",
                                       "TIMETEST DONE", 40))
            for s in SCENARIOS:
                sess.check(f"run{run} timetest {s}",
                           f"timetest: {s} PASS".encode()
                           in sess.buf)
            sess.check(f"run{run} exit clean",
                       b"[exit " not in sess.buf)
            counts.append(free_count(sess))
        sess.check("pmm steady across runs",
                   counts[0] is not None and
                   counts[0] == counts[1])
    finally:
        sess.close()
    print("FAILURES PRESENT" if fails else "ALL OK")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
