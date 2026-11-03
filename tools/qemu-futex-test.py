#!/usr/bin/env python3
"""M50 acceptance: boot and run futextest twice.
WAIT/WAKE basics: value mismatch (EAGAIN), wake with no
waiters, op validation, a timed wait (ETIMEDOUT), and a
no-timeout wait released by a signal (EINTR). Free pages
must be identical across runs.
"""
import os
import re
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK = '/tmp/serFTX'
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
for _ in range(50):
    try:
        s.connect(SOCK)
        break
    except OSError:
        time.sleep(0.2)
s.settimeout(0.5)
buf = b''


def until(needle, timeout=30):
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


def send(cmd):
    global buf
    buf = b''
    s.send((cmd + '\n').encode())
    return until('sh> ')


SCEN = ['mismatch', 'wake-none', 'bad-op', 'timeout',
        'signal', 'xwake', 'join', 'sighand']


def run(tag):
    global buf
    buf = b''
    s.send(b'futextest\n')
    ok = until('FUTEXTEST DONE', 60)
    until('sh> ', 10)  # consume the exit prompt too
    text = buf.decode(errors='replace')
    for name in SCEN:
        check(f'{tag} {name}',
              f'futextest: {name} PASS' in text)
    check(f'{tag} no scenario failed',
          ok and 'FAIL' not in text)
    check(f'{tag} clean exit', '[exit ' not in text)


try:
    until('sh> ')
    run('run1')
    send('cat /proc/meminfo')
    m = re.search(rb'MemFree:\s+(\d+)', buf)
    free1 = int(m.group(1)) // 4 if m else None
    run('run2')
    send('cat /proc/meminfo')
    m = re.search(rb'MemFree:\s+(\d+)', buf)
    free2 = int(m.group(1)) // 4 if m else None
    check('pmm steady across runs',
          free1 is not None and free1 == free2)
finally:
    qemu.kill()

print('FAILURES PRESENT' if fails else 'ALL OK')
sys.exit(1 if fails else 0)
