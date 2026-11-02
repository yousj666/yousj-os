#!/usr/bin/env python3
"""M28 acceptance: boot and run pidtest twice. /proc/<pid>
directories must list live tasks, generate status/stat,
keep zombies visible until reaped and forget reaped
pids. Free pages must be identical across runs.
"""
import os
import re
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK = '/tmp/serM28'
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
for _ in range(100):
    try:
        s.connect(SOCK)
        break
    except OSError:
        time.sleep(0.3)
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


def free_count():
    global buf
    send('free')
    until('sh> ', 20)
    m = re.findall(r'free pages: (\d+)',
                   buf.decode(errors='replace'))
    buf = b''
    return int(m[-1]) if m else None


SCEN = ['root-list', 'self-status', 'self-stat',
        'child-status', 'zombie', 'dead-pid', 'bad-names',
        'piddir-dents', 'stat-modes', 'self-link',
        'cmdline-self', 'cmdline-child', 'environ-self',
        'environ-child', 'cwd-exe', 'maps-self']

check('shell prompt', until('sh> ', 60))
free_counts = []
for run in (1, 2):
    free_counts.append(free_count())
    send('pidtest')
    done = until('PIDTEST DONE', 40)
    seg = buf.decode(errors='replace')
    check(f'run{run} pidtest done', done)
    for name in SCEN:
        check(f'run{run} {name}',
              f'pidtest: {name} PASS' in seg)
    check(f'run{run} no scenario failed',
          'pidtest:' not in seg or ' FAIL' not in seg)
    buf = b''

qemu.terminate()
s.close()
check('pmm steady across runs',
      len(free_counts) == 2 and None not in free_counts and
      free_counts[0] == free_counts[1])
print('ALL OK' if not fails else 'FAILURES PRESENT')
sys.exit(0 if not fails else 1)
