#!/usr/bin/env python3
"""M59 acceptance: Alpine's real busybox on Yousj OS.

busybox 1.36.1 (dynamic musl) ships in the initramfs as
/bin/busybox. Through the system shell we check: the echo
applet, ls over a ramfs directory, busybox sh running a
script file (arithmetic expansion included), a nonzero
exit status, and clean PMM across two rounds.
"""
import os
import re
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK = '/tmp/serBB'
failures = []


def check(name, cond):
    print(('PASS' if cond else 'FAIL'), name)
    if not cond:
        failures.append(name)


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


def until(needle, timeout=60):
    global buf
    end = time.time() + timeout
    while needle.encode() not in buf:
        if time.time() > end:
            return False
        try:
            data = s.recv(65536)
            if data:
                buf += data
        except socket.timeout:
            pass
    return True


def run_cmd(cmd):
    global buf
    buf = b''
    s.send((cmd + '\n').encode())
    until('sh> ')
    return buf.decode(errors='replace')


def memfree():
    text = run_cmd('cat /proc/meminfo')
    m = re.search(r'MemFree:\s+(\d+)', text)
    return int(m.group(1)) if m else None


try:
    ok = until('sh> ')
    check('boot to shell', ok)
    # M63: an interactive busybox sh -- with TCGETS alive
    # it prints its own prompt, edits and runs a command,
    # and exits back to the system shell.
    buf = b''
    s.send(b'busybox sh\n')
    okp = until('/ #')
    check('interactive prompt', okp)
    buf = b''
    s.send(b'echo INBB $((3+4))\n')
    okc = until('INBB 7')
    check('interactive command', okc)
    buf = b''
    s.send(b'exit\n')
    okx = until('sh> ')
    check('interactive exit', okx)
    t = run_cmd('/bin/busybox --install -s /bin')
    check('applet install completes', 'No space' not in t and
          'not an absolute' not in t)
    free0 = None
    for tag in ('run1', 'run2'):
        t = run_cmd('grep BBSCRIPT /bbtest.sh')
        check(tag + ' bare grep applet',
              'echo BBSCRIPT' in t and 'cannot exec' not in t)
        t = run_cmd('wc -c /bbtest.sh')
        check(tag + ' bare wc applet', t.strip().find('44') >= 0)
        t = run_cmd('busybox echo BBHELLO')
        check(tag + ' echo', 'BBHELLO' in t and
              'cannot exec' not in t)
        t = run_cmd('busybox ls /lib')
        check(tag + ' ls', 'ld-musl-x86_64.so.1' in t)
        t = run_cmd('busybox sh /bbtest.sh')
        check(tag + ' script', 'BBSCRIPT 42' in t and
              'NESTED 5' in t)
        t = run_cmd('busybox uname')
        check(tag + ' uname', 'Yousj' in t)
        t = run_cmd('busybox false')
        check(tag + ' exit code', '[exit 1]' in t)
        f = memfree()
        if tag == 'run1':
            free0 = f
        else:
            check('pmm steady across runs',
                  f is not None and f == free0)
finally:
    qemu.kill()

print('FAILURES PRESENT' if failures else 'ALL OK')
sys.exit(1 if failures else 0)
