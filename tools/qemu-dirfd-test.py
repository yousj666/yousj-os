#!/usr/bin/env python3
"""M30 acceptance: boot and run dirfdtest twice (ramfs
tree). dirfd-relative openat/newfstatat/utimensat/
unlinkat/symlinkat must resolve against the directory
the fd was opened on, independently of the cwd. Free
pages must be identical across runs. (The disk suite
runs the same program against /data for the yfs side.)
"""
import os
import re
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK = '/tmp/serM30'
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


SCEN = ['openat-rel', 'openat-sub', 'fdcwd', 'statat-rel',
        'utimensat-rel', 'abs-ignores', 'bad-dfd', 'notdir',
        'unlinkat-rel', 'symlinkat-rel', 'mkdirat-rel',
        'renameat-rel', 'renameat2-noreplace',
        'readlinkat-rel', 'faccessat-rel', 'linkat-eperm',
        'chmod-family', 'creat-modes', 'seekdir', 'sendfile', 'copy-range']

check('shell prompt', until('sh> ', 60))
free_counts = []
for run in (1, 2):
    free_counts.append(free_count())
    send('dirfdtest')
    done = until('DIRFDTEST DONE', 40)
    seg = buf.decode(errors='replace')
    check(f'run{run} dirfdtest done', done)
    for name in SCEN:
        check(f'run{run} {name}',
              f'dirfdtest: {name} PASS' in seg)
    check(f'run{run} no scenario failed',
          'dirfdtest:' not in seg or ' FAIL' not in seg)
    buf = b''

qemu.terminate()
s.close()
check('pmm steady across runs',
      len(free_counts) == 2 and None not in free_counts and
      free_counts[0] == free_counts[1])
print('ALL OK' if not fails else 'FAILURES PRESENT')
sys.exit(0 if not fails else 1)
