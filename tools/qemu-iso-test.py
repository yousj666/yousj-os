#!/usr/bin/env python3
# M80 acceptance: the ISO boots. QEMU gets the OVMF
# firmware and dist/yousj-os-*.iso as a CD-ROM, boot order
# set to the CD: the firmware must use the El Torito
# catalog, load the FAT boot image, and the system must
# come up to a working shell, same as the raw image test.
import glob, os, socket, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(REPO)
ISO = sorted(glob.glob('dist/yousj-os-*.iso'))
assert ISO, 'no dist ISO: run `make iso` first'
ISO = ISO[-1]
TC = os.path.expanduser('~/workspace/toolchain/root/usr/share/OVMF')
SER = '/tmp/serISO'
try:
    os.unlink(SER)
except FileNotFoundError:
    pass
VARS = 'build/iso_vars.fd'
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
    '-drive', 'format=raw,file=' + ISO +
              ',media=cdrom,if=ide,index=2,readonly=on',
    '-boot', 'd',
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

check('firmware boots the ISO (El Torito) and RTC seeds',
      until('RTC wall clock seeded', 60))
check('shell prompt from the ISO', until('sh> ', 40))
mark = len(buf)
send('free')
check('free runs', until('free pages: ', 20, mark))
mark = len(buf)
send('busybox echo iso-boot-ok')
check('busybox runs from the ISO', until('iso-boot-ok', 20, mark))
mark = len(buf)
send('nettest')
check('network stack up from the ISO',
      until('NETTEST DONE', 30, mark))
check('shell back at the end', until('sh> ', 20, mark))
check('no exceptions on the way up', b'EXCEPTION' not in buf)

with open('build/iso_serial_tail.log', 'w') as fh:
    fh.write(buf[-6000:].decode(errors='replace'))
print('ALL OK' if not fails else 'FAILURES PRESENT')
qemu.terminate()
sys.exit(0 if not fails else 1)
