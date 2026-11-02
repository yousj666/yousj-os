import socket, subprocess, sys, time, re, os

# Interactive M10 acceptance: boot the Multiboot chain, run
# /bin/pipetest twice (kernel pipe/dup semantics; the PMM free
# count must be in steady state between runs), then drive the
# shell through its redirection and pipeline syntax:
#   hello > /h.txt   (nothing on the console but [exit 7])
#   cat /h.txt       (the greeting comes back out of the file)
#   hello | cat      (the greeting flows through the pipe)
#   cat < /h.txt     (stdin redirection)
#   echo x > /e.txt  (builtin redirection) + >> append
# Exits non-zero unless ALL OK.

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
for p in ('/tmp/serM10',):
    try: os.unlink(p)
    except FileNotFoundError: pass
qemu = subprocess.Popen(['qemu-system-x86_64','-m','128','-kernel','build/yousj-stub.elf',
    '-initrd','build/yousj-kernel.elf','-display','none',
    '-serial','unix:/tmp/serM10,server,nowait','-monitor','none','-no-reboot'],
    cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(50):
    if os.path.exists('/tmp/serM10'): break
    time.sleep(0.2)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect('/tmp/serM10'); s.settimeout(0.5)
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

SCEN = ['basic', 'block-eof', 'epipe', 'full', 'dup', 'cloexec']
check('shell prompt', until('sh> ', 45))

def run_suite(tag):
    mark = len(buf)
    send('pipetest')
    check(tag + ' suite done', until('PIPETEST DONE', 60, mark))
    seg = buf[mark:].decode(errors='replace')
    for name in SCEN:
        check(tag + ' pipetest ' + name, ('PIPETEST ' + name + ' PASS') in seg)
    check(tag + ' pipetest exit 0', until('sh> ', 10, mark)
          and '[exit ' not in seg.rsplit('PIPETEST DONE', 1)[1])
    return free_pages(len(buf))

f1 = run_suite('run1')
f2 = run_suite('run2')
check('PMM steady state (run1 == run2)', f1 is not None and f1 == f2)

# shell redirection: stdout of hello goes to the file only
mark = len(buf)
send('hello > /h.txt')
check('redir: prompt back', until('sh> ', 10, mark))
seg = buf[mark:].decode(errors='replace')
check('redir: greeting NOT on console',
      'hello from Yousj userland' not in seg)
check('redir: hello exit 7 noted', '[exit 7]' in seg)

mark = len(buf)
send('cat /h.txt')
check('cat file: greeting read back',
      until('hello from Yousj userland', 10, mark))
check('cat file: prompt back', until('sh> ', 10, mark))

# pipeline: hello | cat
mark = len(buf)
send('hello | cat')
check('pipe: greeting through cat',
      until('hello from Yousj userland', 10, mark))
seg = buf[mark:].decode(errors='replace')
check('pipe: no [exit (status is cat, 0)', '[exit ' not in seg)
check('pipe: prompt back', until('sh> ', 10, mark))

# stdin redirection
mark = len(buf)
send('cat < /h.txt')
check('stdin redir: greeting shown',
      until('hello from Yousj userland', 10, mark))
check('stdin redir: prompt back', until('sh> ', 10, mark))

# builtin echo redirection + append
mark = len(buf)
send('echo redir-works > /e.txt')
check('echo redir: prompt back', until('sh> ', 10, mark))
seg = buf[mark:].decode(errors='replace')
check('echo redir: text NOT on console', '\nredir-works\n' not in seg)
mark = len(buf)
send('echo second-line >> /e.txt')
check('echo append: prompt back', until('sh> ', 10, mark))
mark = len(buf)
send('cat /e.txt')
check('echo file: first line', until('redir-works', 10, mark))
check('echo file: appended line', until('second-line', 10, mark))
check('echo file: prompt back', until('sh> ', 10, mark))

print('ALL OK' if not fails else 'FAILURES PRESENT')
qemu.terminate()
sys.exit(0 if not fails else 1)
