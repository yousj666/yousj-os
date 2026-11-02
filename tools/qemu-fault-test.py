#!/usr/bin/env python3
"""M24 acceptance: boot (no disk needed) and run faulttest
twice from the shell. All scenarios PASS in both runs and
the free-page count is identical across runs.

Checks: file-ready, pipe-timeout, pipe-data-hup,
pipe-write, select-basic, eintr.
"""
import os
import re
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK = '/tmp/serM24'
SCENARIOS = ["rusage-ro", "read-ro", "read-wild", "stat-ro", "sleep-wild", "tod-ro", "sigret-wild", "alive"]
fails = []


def check(name, cond):
    print(('PASS' if cond else 'FAIL'), name, flush=True)
    if not cond:
        fails.append(name)


try:
    os.unlink(SOCK)
except FileNotFoundError:
    pass
qemu = subprocess.Popen(
    ['qemu-system-x86_64', '-m', '128',
     '-kernel', 'build/yousj-stub.elf',
     '-initrd', 'build/yousj-kernel.elf',
     '-display', 'none',
     '-serial', f'unix:{SOCK},server,nowait',
     '-monitor', 'none', '-no-reboot'],
    cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(50):
    if os.path.exists(SOCK):
        break
    time.sleep(0.2)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(SOCK)
s.settimeout(0.5)
buf = b''


def until(needle, timeout=40):
    global buf
    end = time.time() + timeout
    while needle.encode() not in buf:
        if time.time() > end:
            return False
        try:
            d = s.recv(65536)
            if d:
                buf += d
        except socket.timeout:
            pass
    return True


def send(line):
    global buf
    until('sh> ', 20)
    buf = buf.split(b'sh> ')[-1]
    s.send((line + '\n').encode())
    time.sleep(0.3)


check('shell prompt', until('sh> ', 60))
free_counts = []
for run in (1, 2):
    send('faulttest')
    done = until('FAULTTEST DONE', 40)
    seg = buf.decode(errors='replace')
    check(f'run{run} faulttest done', done)
    for name in SCENARIOS:
        check(f'run{run} faulttest {name}',
              f'faulttest: {name} PASS' in seg)
    buf = b''
    send('free')
    until('sh> ', 20)
    m = re.findall(r'free pages: (\d+)',
                   buf.decode(errors='replace'))
    check(f'run{run} free read', len(m) > 0)
    if m:
        free_counts.append(int(m[-1]))
    buf = b''

qemu.terminate()
s.close()
check('pmm steady across runs',
      len(free_counts) == 2 and free_counts[0] == free_counts[1])
print('ALL OK' if not fails else 'FAILURES PRESENT')
sys.exit(0 if not fails else 1)
