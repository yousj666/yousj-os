#!/usr/bin/env python3
# M78 acceptance: the distributable image boots on its own.
# QEMU gets ONLY the OVMF firmware and dist/yousj-os-*.img
# as a disk -- no -kernel, no -initrd, no multiboot. The
# firmware must find BOOTX64.EFI in the ESP, the loader
# must find YOUSJ.KRN next to it, and the system must come
# up to a working shell with the network stack alive.
import glob, os, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(REPO)
IMG = sorted(glob.glob('dist/yousj-os-*.img'))
assert IMG, 'no dist image: run `make image` first'
IMG = IMG[-1]
TC = os.path.expanduser('~/workspace/toolchain/root/usr/share/OVMF')
SER = '/tmp/serIMG'
try:
    os.unlink(SER)
except FileNotFoundError:
    pass
VARS = 'build/image_vars.fd'
subprocess.run(['cp', os.path.join(TC, 'OVMF_VARS_4M.fd'), VARS],
               check=True)

fails = []

def check(name, cond):
    print(('PASS ' if cond else 'FAIL ') + name)
    if not cond:
        fails.append(name)

qemu = subprocess.Popen([
    'qemu-system-x86_64', '-machine', 'q35', '-m', '256',
    '-drive', 'if=pflash,format=raw,readonly=on,file=' +
              os.path.join(TC, 'OVMF_CODE_4M.fd'),
    '-drive', 'if=pflash,format=raw,file=' + VARS,
    '-drive', 'format=raw,file=' + IMG + ',if=ide,index=2',
    '-display', 'none',
    '-serial', 'unix:%s,server,nowait' % SER,
    '-monitor', 'none', '-no-reboot',
    '-netdev', 'user,id=n0',
    '-device', 'virtio-net-pci,disable-modern=on,netdev=n0',
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(50):
    try:
        s.connect(SER)
        break
    except OSError:
        time.sleep(0.2)
s.settimeout(0.5)
buf = b''

def until(needle, timeout=60, start=0):
    global buf
    end = time.time() + timeout
    while needle.encode() not in buf[start:]:
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
    s.send((line + '\n').encode())
    time.sleep(0.3)

check('firmware hands off and RTC seeds',
      until('RTC wall clock seeded', 60))
check('shell prompt from the image alone', until('sh> ', 40))
mark = len(buf)
send('free')
check('free runs', until('free pages: ', 20, mark))
mark = len(buf)
send('busybox echo image-boot-ok')
check('busybox runs from the image',
      until('image-boot-ok', 20, mark))
mark = len(buf)
send('nettest')
check('network stack up in the image',
      until('NETTEST DONE', 30, mark))
check('shell back at the end', until('sh> ', 20, mark))
check('no exceptions on the way up',
      b'EXCEPTION' not in buf)

with open('build/image_serial_tail.log', 'w') as fh:
    fh.write(buf[-6000:].decode(errors='replace'))
print('ALL OK' if not fails else 'FAILURES PRESENT')
qemu.terminate()
sys.exit(0 if not fails else 1)
