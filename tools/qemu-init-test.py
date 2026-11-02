import socket, subprocess, sys, time, re, os

# Interactive M4 acceptance: boot the Multiboot chain with the
# serial console on a unix socket, then drive init's command
# loop -- help, ps (demo-sleeper + init listed), kill the
# sleeper, ps again (sleeper gone), exit (kernel panics with
# 'Attempted to kill init'). Exits non-zero unless ALL OK.

for p in ('/tmp/serX',):
    try: os.unlink(p)
    except FileNotFoundError: pass
qemu = subprocess.Popen(['qemu-system-x86_64','-m','128','-kernel','build/yousj-stub.elf',
    '-initrd','build/yousj-kernel.elf','-display','none',
    '-serial','unix:/tmp/serX,server,nowait','-monitor','none','-no-reboot'],
    cwd=os.path.dirname(os.path.dirname(os.path.abspath(__file__))), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(50):
    if os.path.exists('/tmp/serX'): break
    time.sleep(0.2)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect('/tmp/serX'); s.settimeout(0.5)
buf = b''
def until(needle, timeout=20, start=0):
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

ok = True
def check(name, cond):
    global ok
    print(('PASS' if cond else 'FAIL'), name)
    ok = ok and cond

check('boot to prompt', until('init> ', 45))
mark = len(buf); send('help')
check('help responds', until('built-ins:', 8, mark))
seg = buf[mark:].decode(errors='replace')
check('help lists kill', 'kill <tid>' in seg)
mark = len(buf); send('ps')
check('ps responds', until('init> ', 8, mark))
seg = buf[mark:].decode(errors='replace')
m = re.search(r'(\d+)\s+\S\s+demo-sleeper', seg)
check('ps shows demo-sleeper', m is not None)
check('ps shows init', re.search(r'\d+\s+\S\s+init', seg) is not None)
tid = m.group(1) if m else '0'
mark = len(buf); send(f'kill {tid}')
check('kill responds', until('init> ', 8, mark))
mark = len(buf); send('ps')
until('init> ', 8, mark)
seg = buf[mark:].decode(errors='replace')
check('sleeper gone after kill', 'demo-sleeper' not in seg)
mark = len(buf); send('exit')
check('init exit panics', until('Attempted to kill init', 8, mark))
print('ALL OK' if ok else 'SOME FAILED')
qemu.kill()
