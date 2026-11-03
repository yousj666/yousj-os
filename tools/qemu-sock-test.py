import socket, subprocess, sys, time, re, os

# Interactive M68 acceptance: boot the Multiboot chain and run
# /bin/socktest twice (named AF_UNIX stream sockets: bind,
# listen, connect, accept, backlog, close semantics, and the
# fork-shaped client/server exchange). The PMM free count
# must be in steady state between runs. ALL OK or exit 1.

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for p in ('/tmp/serSOCK',):
    try: os.unlink(p)
    except FileNotFoundError: pass
qemu = subprocess.Popen(['qemu-system-x86_64','-m','128','-kernel','build/yousj-stub.elf',
    '-initrd','build/yousj-kernel.elf','-display','none',
    '-serial','unix:/tmp/serSOCK,server,nowait','-monitor','none','-no-reboot'],
    cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(50):
    if os.path.exists('/tmp/serSOCK'): break
    time.sleep(0.2)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect('/tmp/serSOCK'); s.settimeout(0.5)
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
    return int(m.group(1)) if m else None

fails = []
def check(name, cond):
    print(('PASS ' if cond else 'FAIL ') + name)
    if not cond: fails.append(name)

SCEN = ['reject', 'bindstat', 'refused', 'e2e', 'queue', 'nbaccept', 'close', 'fork', 'msgio', 'dgram']
check('shell prompt', until('sh> ', 45))

def run_suite(tag):
    mark = len(buf)
    send('socktest')
    check(tag + ' suite done', until('SOCKTEST DONE', 60, mark))
    seg = buf[mark:].decode(errors='replace')
    for name in SCEN:
        check(tag + ' socktest ' + name, ('SOCKTEST ' + name + ' PASS') in seg)
    check(tag + ' socktest exit 0', until('sh> ', 10, mark)
          and '[exit ' not in seg.rsplit('SOCKTEST DONE', 1)[1])
    return free_pages(len(buf))

f1 = run_suite('run1')
f2 = run_suite('run2')
check('PMM steady state (run1 == run2)', f1 is not None and f1 == f2)

print('ALL OK' if not fails else 'FAILURES PRESENT')
qemu.terminate()
sys.exit(0 if not fails else 1)
