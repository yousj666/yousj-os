import socket, subprocess, sys, time, re, os

# Interactive M9 acceptance: boot the Multiboot chain with the
# serial console on a unix socket, run /bin/fstest in the shell
# TWICE. Every scenario must PASS in both runs (the ramfs is
# system-global: run 2 must see run 1's files), and the PMM
# free-page count after run 2 must equal the count after run 1
# (steady state: fixed filenames, truncate/rewrite -- no leak).
# Exits non-zero unless ALL OK.

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for p in ('/tmp/serM9',):
    try: os.unlink(p)
    except FileNotFoundError: pass
qemu = subprocess.Popen(['qemu-system-x86_64','-m','128','-kernel','build/yousj-stub.elf',
    '-initrd','build/yousj-kernel.elf','-display','none',
    '-serial','unix:/tmp/serM9,server,nowait','-monitor','none','-no-reboot'],
    cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(50):
    if os.path.exists('/tmp/serM9'): break
    time.sleep(0.2)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect('/tmp/serM9'); s.settimeout(0.5)
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

SCENARIOS = ['persist', 'write-read', 'append', 'trunc', 'seek',
             'dents', 'errors', 'fork-share', 'mkdir-cwd', 'openat',
             'unlink', 'unlink-open', 'rename', 'rmdir',
             'symlink', 'link-chain', 'link-loop', 'link-dangle',
             'readlink', 'link-midpath', 'link-unlink']

check('shell prompt', until('sh> ', 45))

def run_suite(tag):
    mark = len(buf)
    send('fstest')
    ok_done = until('FSTEST DONE', 60, mark)
    check(tag + ' suite finished', ok_done)
    seg = buf[mark:].decode(errors='replace')
    for name in SCENARIOS:
        check(tag + ' fstest ' + name, ('FSTEST ' + name + ' PASS') in seg)
    check(tag + ' fstest exited 0',
          until('sh> ', 10, mark) and 'FSTEST DONE' in seg
          and '[exit ' not in seg.rsplit('FSTEST DONE', 1)[1].split('free')[0])
    smark = len(buf)
    send('stattest /stx')
    ok_sdone = until('STATTEST DONE', 60, smark)
    check(tag + ' stattest finished', ok_sdone)
    sseg = buf[smark:].decode(errors='replace')
    for name in ['file', 'link', 'fstatat', 'dir-missing',
                 'console', 'empty-path']:
        check(tag + ' stattest ' + name,
              ('stattest: ' + name + ' PASS') in sseg)
    fmark = len(buf)
    f = free_pages(fmark)
    check(tag + ' free read', f is not None)
    return f

f1 = run_suite('run1')
f2 = run_suite('run2')
check('PMM steady state (run1 == run2)', f1 is not None and f1 == f2)

mark = len(buf); send('ps')
check('ps returns to prompt', until('sh> ', 8, mark))

# M11: /etc/motd rode in with the initramfs (a data file, and
# the shell already printed it at startup)
seg_all = buf.decode(errors='replace')
check('motd printed at shell start',
      'Welcome to Yousj OS' in seg_all.split('shell ready')[0]
      or 'Welcome to Yousj OS' in seg_all)
mark = len(buf); send('cat /etc/motd')
check('cat /etc/motd works',
      until('initramfs image', 10, mark))
check('motd: prompt back', until('sh> ', 8, mark))
print('ALL OK' if not fails else 'FAILURES PRESENT')
qemu.terminate()
sys.exit(0 if not fails else 1)
