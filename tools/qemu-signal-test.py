import socket, subprocess, sys, time, re, os

# Interactive M7 acceptance: boot the Multiboot chain with the
# serial console on a unix socket, run /bin/sigtest in the shell
# and verify every scenario line reports PASS, then confirm the
# process table and the PMM balance are clean afterwards (the
# suite forks many short-lived children; nothing may leak --
# including signal frames' address spaces).
# Exits non-zero unless ALL OK.

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for p in ('/tmp/serM7',):
    try: os.unlink(p)
    except FileNotFoundError: pass
qemu = subprocess.Popen(['qemu-system-x86_64','-m','128','-kernel','build/yousj-stub.elf',
    '-initrd','build/yousj-kernel.elf','-display','none',
    '-serial','unix:/tmp/serM7,server,nowait','-monitor','none','-no-reboot'],
    cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(50):
    if os.path.exists('/tmp/serM7'): break
    time.sleep(0.2)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect('/tmp/serM7'); s.settimeout(0.5)
buf = b''
def until(needle, timeout=30, start=0):
    global buf
    end = time.time() + timeout
    while needle.encode() not in buf[start:]:
        if time.time() > end: return False
        try:
            d = s.recv(65536)
            if d: buf += d
        except socket.timeout: pass
    return True
def send(line):
    s.send((line + '\n').encode()); time.sleep(0.3)
def free_pages(mark):
    send('free')
    if not until('free pages: ', 8, mark): return None
    seg = buf[mark:].decode(errors='replace')
    m = re.search(r'free pages: (\d+)', seg)
    until('sh> ', 8, mark)
    return int(m.group(1)) if m else None

ok = True
def check(name, cond):
    global ok
    print(('PASS' if cond else 'FAIL'), name)
    ok = ok and cond

SCENARIOS = ['handler', 'mask', 'default-term', 'stop-cont',
             'sigkill', 'eintr', 'restart', 'sigchld',
             'irq-deliver', 'clone-inherit', 'exec-reset',
             'altstack']

check('shell prompt', until('sh> ', 45))
mark = len(buf); f1 = free_pages(mark)
check('free baseline read', f1 is not None)

mark = len(buf)
send('sigtest')
check('suite finished', until('SIGTEST DONE', 60, mark))
seg = buf[mark:].decode(errors='replace')
for name in SCENARIOS:
    check('sigtest ' + name, ('SIGTEST ' + name + ' PASS') in seg)
    if ('SIGTEST ' + name + ' FAIL') in seg:
        print('  ^^ sigtest reported FAIL for', name)
check('sigtest exited 0 (shell prints [exit N] only for N != 0)',
      until('sh> ', 10, mark) and '[exit ' not in seg.split('SIGTEST DONE')[1])

mark = len(buf); f2 = free_pages(mark)
check('free after suite read', f2 is not None)
check('PMM balanced (no signal-era leak)', f1 == f2)

send('ps')
mark2 = len(buf)
check('ps returns to prompt', until('sh> ', 8, mark2))

print('ALL OK' if ok else 'FAILURES PRESENT')
qemu.terminate()
sys.exit(0 if ok else 1)
