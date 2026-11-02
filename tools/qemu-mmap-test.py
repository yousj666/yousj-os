#!/usr/bin/env python3
"""M14 acceptance: boot with a blank data disk (so /data is
a live yfs volume) and run mmtest twice from the shell.
Every scenario must PASS in both runs, and the PMM free-page
count printed by `free` afterwards must be identical across
runs -- mmap/munmap cycles must not leak frames.

Checks (mmtest): anon, priv-read, priv-write, shared-ramfs,
shared-yfs, offset.
"""
import os
import re
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DISK = os.path.join(REPO, 'build', 'mmap-test.img')
SOCK = '/tmp/serM14'
SCENARIOS = ['anon', 'priv-read', 'priv-write', 'shared-ramfs',
             'shared-yfs', 'offset']
fails = []


def check(name, cond):
    print(('PASS' if cond else 'FAIL'), name, flush=True)
    if not cond:
        fails.append(name)


def boot():
    try:
        os.unlink(SOCK)
    except FileNotFoundError:
        pass
    qemu = subprocess.Popen(
        ['qemu-system-x86_64', '-m', '128',
         '-kernel', 'build/yousj-stub.elf',
         '-initrd', 'build/yousj-kernel.elf',
         '-drive', f'file={DISK},format=raw,if=ide,index=0',
         '-display', 'none',
         '-serial', f'unix:{SOCK},server,nowait',
         '-monitor', 'none', '-no-reboot'],
        cwd=REPO, stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL)
    for _ in range(50):
        if os.path.exists(SOCK):
            break
        time.sleep(0.2)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.settimeout(0.5)
    return qemu, s


class Session:
    def __init__(self, s):
        self.s = s
        self.buf = b''

    def until(self, needle, timeout=40):
        end = time.time() + timeout
        while needle.encode() not in self.buf:
            if time.time() > end:
                return False
            try:
                d = self.s.recv(65536)
                if d:
                    self.buf += d
            except socket.timeout:
                pass
        return True

    def send(self, line):
        self.until('sh> ', 20)
        self.buf = self.buf.split(b'sh> ')[-1]
        self.s.send((line + '\n').encode())
        time.sleep(0.3)


with open(DISK, 'wb') as f:
    f.write(b'\0' * (16 * 1024 * 1024))
qemu, s = boot()
sess = Session(s)
check('shell prompt', sess.until('sh> ', 60))
check('yfs mounted', b'yfs formatted and mounted' in sess.buf)

free_counts = []
for run in (1, 2):
    sess.buf = b''
    sess.send('mmtest')
    done = sess.until('MMTEST DONE', 30)
    seg = sess.buf.decode(errors='replace')
    check(f'run{run} mmtest done', done)
    for name in SCENARIOS:
        check(f'run{run} mmtest {name}',
              f'mmtest: {name} PASS' in seg)
    sess.buf = b''
    sess.send('free')
    ok = sess.until('sh> ', 20)
    m = re.findall(r'free pages: (\d+)',
                   sess.buf.decode(errors='replace'))
    check(f'run{run} free read', ok and len(m) > 0)
    if m:
        free_counts.append(int(m[-1]))

qemu.terminate()
s.close()
os.unlink(DISK)
check('pmm steady across runs',
      len(free_counts) == 2 and free_counts[0] == free_counts[1])
print('ALL OK' if not fails else 'FAILURES PRESENT')
sys.exit(0 if not fails else 1)
