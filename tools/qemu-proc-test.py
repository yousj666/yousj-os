import socket, subprocess, sys, time, re, os

# Interactive M5 acceptance: boot the Multiboot chain with the
# serial console on a unix socket, then verify the process
# family end to end --
#   * supervisor init spawns the shell (clone + execve)
#   * the shell runs /bin/hello and reports [exit 7]
#   * PMM free-page count balances across repeated command
#     rounds (clone copy + exec + teardown + reap leaks nothing)
#   * shell exit -> init reaps it, prints INIT_SMOKE_OK and
#     respawns a fresh shell
# Exits non-zero unless ALL OK.

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for p in ('/tmp/serM5',):
    try: os.unlink(p)
    except FileNotFoundError: pass
qemu = subprocess.Popen(['qemu-system-x86_64','-m','128','-kernel','build/yousj-stub.elf',
    '-initrd','build/yousj-kernel.elf','-display','none',
    '-serial','unix:/tmp/serM5,server,nowait','-monitor','none','-no-reboot'],
    cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(50):
    if os.path.exists('/tmp/serM5'): break
    time.sleep(0.2)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect('/tmp/serM5'); s.settimeout(0.5)
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

check('shell prompt', until('sh> ', 45))
mark = len(buf); f1 = free_pages(mark)
check('free baseline read', f1 is not None)
for i in range(3):
    mark = len(buf); send('hello')
    check(f'hello round {i+1}', until('[exit 7]', 10, mark))
mark = len(buf); f2 = free_pages(mark)
check('free after rounds read', f2 is not None)
check(f'PMM balanced ({f1} -> {f2})', f1 is not None and f1 == f2)
mark = len(buf); send('ps')
check('ps lists the shell', until('sh', 8, mark))
mark = len(buf); send('exit')
check('init reaps shell', until('yousj-init: child exited code=0', 10, mark))
check('smoke marker', until('yousj-init: INIT_SMOKE_OK', 5, mark))
check('shell respawned', until('yousj-sh: shell ready', 10, mark))
mark = len(buf); send('echo alive-again')
check('respawned shell works', until('alive-again', 10, mark))
print('ALL OK' if ok else 'SOME FAILED')
qemu.kill()
sys.exit(0 if ok else 1)
