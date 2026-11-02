import socket, subprocess, sys, time, os

# Interactive M12 acceptance: TRUE persistence across a hard
# power cut.
#   Phase 1: boot with a fresh zeroed 16 MiB IDE disk. The
#     kernel must format + mount yfs at /data. Write files via
#     shell redirection and mkdir, read them back, then KILL
#     QEMU (no shutdown -- a power cut).
#   Phase 2: boot again with the same disk image. The kernel
#     must mount the existing filesystem (not reformat) and
#     every file must still be there; an append must land
#     after the phase-1 content.
# The disk image lives in build/ and is removed at the end.

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DISK = os.path.join(REPO, 'build', 'disk-test.img')
SOCK = '/tmp/serM12'

fails = []
def check(name, cond):
    print(('PASS ' if cond else 'FAIL ') + name)
    if not cond: fails.append(name)

def boot():
    try: os.unlink(SOCK)
    except FileNotFoundError: pass
    qemu = subprocess.Popen(
        ['qemu-system-x86_64', '-m', '128',
         '-kernel', 'build/yousj-stub.elf',
         '-initrd', 'build/yousj-kernel.elf',
         '-drive', 'file=' + DISK + ',format=raw,if=ide,index=0',
         '-display', 'none',
         '-serial', 'unix:' + SOCK + ',server,nowait',
         '-monitor', 'none', '-no-reboot'],
        cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(50):
        if os.path.exists(SOCK): break
        time.sleep(0.2)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.settimeout(0.5)
    return qemu, s

class Session:
    def __init__(self, s):
        self.s = s
        self.buf = b''
    def until(self, needle, timeout=40):
        end = time.time() + timeout
        while needle.encode() not in self.buf:
            if time.time() > end: return False
            try:
                d = self.s.recv(65536)
                if d: self.buf += d
            except socket.timeout: pass
        return True
    def send(self, line):
        # wait for a fresh prompt first so commands never merge
        self.until('sh> ', 20)
        self.buf = self.buf.split(b'sh> ')[-1]
        self.s.send((line + '\n').encode())
        time.sleep(0.3)
    def run_expect(self, line, needle, timeout=25):
        self.send(line)
        return self.until(needle, timeout)

# ---------- phase 1: format + write + power cut ----------
with open(DISK, 'wb') as f:
    f.write(b'\0' * (16 * 1024 * 1024))
qemu, s = boot()
sess = Session(s)
check('p1 shell prompt', sess.until('sh> ', 60))
check('p1 yfs formatted',
      b'yfs formatted and mounted' in sess.buf)
check('p1 write via echo >',
      sess.run_expect('echo persist-me > /data/keep.txt', 'sh> '))
sess.buf = b''
check('p1 read back', sess.run_expect('cat /data/keep.txt', 'persist-me'))
check('p1 mkdir', sess.run_expect('mkdir /data/sub', 'sh> '))
sess.buf = b''
check('p1 write in subdir',
      sess.run_expect('echo nested > /data/sub/a.txt', 'sh> '))
sess.buf = b''
check('p1 subdir read back',
      sess.run_expect('cat /data/sub/a.txt', 'nested'))
check('p1 hello onto disk',
      sess.run_expect('hello > /data/h.txt', 'sh> '))
sess.buf = b''
check('p1 hello read back',
      sess.run_expect('cat /data/h.txt', 'hello from Yousj userland'))
qemu.kill()  # power cut
qemu.wait()
s.close()

# ---------- phase 2: remount + verify + append ----------
qemu, s = boot()
sess = Session(s)
check('p2 shell prompt', sess.until('sh> ', 60))
check('p2 yfs mounted existing',
      b'yfs mounted on /data' in sess.buf and
      b'formatted' not in sess.buf)
sess.buf = b''
check('p2 keep.txt survived',
      sess.run_expect('cat /data/keep.txt', 'persist-me'))
check('p2 h.txt survived',
      sess.run_expect('cat /data/h.txt', 'hello from Yousj userland'))
check('p2 subdir file survived',
      sess.run_expect('cat /data/sub/a.txt', 'nested'))
sess.buf = b''
check('p2 ls /data lists keep.txt',
      sess.run_expect('ls /data', 'keep.txt'))
seg = sess.buf.decode(errors='replace')
check('p2 ls /data lists sub/ and h.txt',
      'sub/' in seg and 'h.txt' in seg)
sess.buf = b''
check('p2 append', sess.run_expect('echo second-boot >> /data/keep.txt', 'sh> '))
sess.buf = b''
ok = sess.run_expect('cat /data/keep.txt', 'second-boot')
seg = sess.buf.decode(errors='replace')
check('p2 append after original',
      ok and seg.find('persist-me') >= 0 and
      seg.find('persist-me') < seg.find('second-boot'))
qemu.terminate()
s.close()
os.unlink(DISK)
print('ALL OK' if not fails else 'FAILURES PRESENT')
sys.exit(0 if not fails else 1)
