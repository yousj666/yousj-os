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
sess.buf = b''
check('p2 bigfile write 100KiB',
      sess.run_expect('bigfile /data/big.bin 102400', 'sh> '))
sess.buf = b''
check('p2 bigfile verify',
      sess.run_expect('bigfile /data/big.bin 102400 check',
                      'sh> '))
sess.buf = b''
check('p2 rename keep->renamed',
      sess.run_expect('mv /data/keep.txt /data/renamed.txt',
                      'sh> '))
sess.buf = b''
sess.run_expect('ln -s renamed.txt /data/k-link', 'sh> ')
check('p2 rm h.txt', sess.run_expect('rm /data/h.txt', 'sh> '))
sess.buf = b''
ok = sess.run_expect('stattest /data/stx', 'STATTEST DONE')
seg = sess.buf.decode(errors='replace')
check('p2 stattest on yfs (all six, no FAIL)',
      ok and 'stattest: link PASS' in seg and
      'stattest: empty-path PASS' in seg and
      'FAIL' not in seg)
sess.buf = b''
ok = sess.run_expect('timetest /data/tt-file', 'TIMETEST DONE', 60)
seg = sess.buf.decode(errors='replace')
check('p2 timetest on yfs (all six, no FAIL)',
      ok and 'timetest: mtime PASS' in seg and
      'timetest: clocks PASS' in seg and
      'FAIL' not in seg)
sess.buf = b''
ok = sess.run_expect('utimetest /data/ut-file alias', 'UTIMETEST DONE', 60)
seg = sess.buf.decode(errors='replace')
check('p2 utimetest on yfs (alias mode, no FAIL)',
      ok and 'utimetest: set-explicit PASS' in seg and
      'utimetest: errors PASS' in seg and
      'FAIL' not in seg)
qemu.kill()  # power cut again
qemu.wait()
s.close()

# ---------- phase 3: rename/delete/big file persisted ----------
qemu, s = boot()
sess = Session(s)
check('p3 shell prompt', sess.until('sh> ', 60))
check('p3 yfs mounted existing',
      b'yfs mounted on /data' in sess.buf and
      b'formatted' not in sess.buf)
sess.buf = b''
ok = sess.run_expect('cat /data/renamed.txt', 'persist-me')
ok = ok and sess.until('second-boot', 15)
seg = sess.buf.decode(errors='replace')
check('p3 renamed.txt has both lines',
      ok and 'second-boot' in seg)
sess.buf = b''
check('p3 ls runs', sess.run_expect('ls /data', 'renamed.txt'))
seg = sess.buf.decode(errors='replace')
check('p3 ls: renamed+big+sub, no h.txt',
      'renamed.txt' in seg and 'big.bin' in seg and
      'sub/' in seg and 'h.txt' not in seg)
sess.buf = b''
check('p3 bigfile verify after power cut',
      sess.run_expect('bigfile /data/big.bin 102400 check',
                      'sh> '))
sess.buf = b''
ok = sess.run_expect('cat /data/k-link', 'persist-me')
ok = ok and sess.until('second-boot', 15)
check('p3 symlink to renamed.txt reads through', ok)
check('p3 readlink prints target',
      sess.run_expect('readlink /data/k-link', 'renamed.txt'))
check('p3 subdir file still there',
      sess.run_expect('cat /data/sub/a.txt', 'nested'))
qemu.terminate()
qemu.wait()
s.close()

# ---------- M19: journal must be clear after clean ops ----
with open(DISK, 'rb') as f:
    f.seek(25 * 512)
    jhdr = f.read(512)
check('jrnl header present after p3',
      jhdr[0:8] == b'YOUSJJR1')
import struct as _st
check('jrnl count is 0 after p3 (all txns checkpointed)',
      _st.unpack_from('<I', jhdr, 12)[0] == 0)

# ---------- phase 4: churn + hard kill mid-transaction ----
qemu, s = boot()
sess = Session(s)
check('p4 shell prompt', sess.until('sh> ', 60))
check('p4 yfs mounted existing',
      b'yfs mounted on /data' in sess.buf)
sess.buf = b''
check('p4 sentinel write',
      sess.run_expect('echo sentinel-ok > /data/sentinel.txt',
                      'sh> '))
sess.buf = b''
check('p4 sentinel read',
      sess.run_expect('cat /data/sentinel.txt', 'sentinel-ok'))
sess.until('sh> ', 20)
sess.buf = b''
s.send(b'churn\n')
cycles = 0
end = time.time() + 90
while time.time() < end:
    cycles = sess.buf.count(b'churn cycle')
    if cycles >= 3:
        break
    try:
        d = s.recv(65536)
        if d: sess.buf += d
    except socket.timeout: pass
check('p4 churn ran >=3 cycles', cycles >= 3)
time.sleep(1.5)
qemu.kill()  # power cut mid-transaction, hopefully
qemu.wait()
s.close()

# ---------- phase 5: structure survives the churn kill ----
qemu, s = boot()
sess = Session(s)
check('p5 shell prompt', sess.until('sh> ', 60))
check('p5 yfs mounted existing',
      b'yfs mounted on /data' in sess.buf and
      b'formatted' not in sess.buf)
sess.buf = b''
check('p5 sentinel intact',
      sess.run_expect('cat /data/sentinel.txt', 'sentinel-ok'))
sess.buf = b''
check('p5 ls /data works',
      sess.run_expect('ls /data', 'sentinel.txt'))
sess.buf = b''
check('p5 probe write',
      sess.run_expect('echo probe > /data/probe.txt', 'sh> '))
sess.buf = b''
check('p5 probe read',
      sess.run_expect('cat /data/probe.txt', 'probe'))
check('p5 probe rm', sess.run_expect('rm /data/probe.txt',
                                     'sh> '))
sess.buf = b''
check('p5 bigfile still verifies',
      sess.run_expect('bigfile /data/big.bin 102400 check',
                      'sh> '))
qemu.terminate()
s.close()
os.unlink(DISK)
print('ALL OK' if not fails else 'FAILURES PRESENT')
sys.exit(0 if not fails else 1)
