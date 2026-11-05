#!/usr/bin/env python3
# M92 acceptance: the DHCP client. The guest boots behind a
# slirp whose network is NOT the compiled-in default: subnet
# 192.168.50.0/24, DHCP pool starting at .100, host at .2.
# A lease of 192.168.50.100 therefore cannot be the static
# fallback -- it can only have come out of a real
# DISCOVER/OFFER/REQUEST/ACK exchange. After the lease, the
# stack must work on the new configuration: the netinfo
# snapshot shows the leased values, and a ping to the NEW
# gateway (ARP on the new subnet, ICMP to the new address,
# reply addressed to the leased IP) comes back.
import socket, subprocess, sys, time, re, os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(REPO)
SER = '/tmp/serDHCP'
try:
    os.unlink(SER)
except FileNotFoundError:
    pass

fails = []

def check(name, cond):
    print(('PASS ' if cond else 'FAIL ') + name)
    if not cond:
        fails.append(name)

qemu = subprocess.Popen([
    'qemu-system-x86_64', '-m', '128',
    '-kernel', 'build/yousj-stub.elf',
    '-initrd', 'build/yousj-kernel.elf',
    '-display', 'none',
    '-serial', 'unix:%s,server,nowait' % SER,
    '-monitor', 'none', '-no-reboot',
    '-netdev', 'user,id=n0,net=192.168.50.0/24,'
               'dhcpstart=192.168.50.100,host=192.168.50.2',
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

def until(needle, timeout=45, start=0):
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

def free_pages(mark):
    send('free')
    if not until('free pages: ', 20, mark):
        return None
    seg = buf[mark:].decode(errors='replace')
    m = re.search(r'free pages: (\d+)', seg)
    return int(m.group(1)) if m else None

check('link up on the foreign net', until('net: link up', 45))
check('dhcp lease of the pool address',
      until('dhcp lease ip=192.168.50.100 mask=255.255.255.0 '
            'gw=192.168.50.2', 45))
check('shell prompt', until('sh> ', 45))
f0 = free_pages(len(buf))

mark = len(buf)
send('nettest')
ok = until('NETTEST DONE', 30, mark)
seg = buf[mark:].decode(errors='replace')
check('nettest ran', ok)
check('snapshot shows leased ip', 'ip=192.168.50.100' in seg)
check('snapshot shows dhcp=lease', 'dhcp=lease' in seg)
check('snapshot shows leased gw', 'gw=192.168.50.2' in seg)

mark = len(buf)
send('nettest ping 192.168.50.2')
ok = until('NETTEST DONE', 30, mark)
seg = buf[mark:].decode(errors='replace')
check('ping to the new gateway sent',
      ok and 'NETTEST ping-sent' in seg)
m = re.search(r'ping_replied=(\d+)', seg)
check('ping reply came back on the leased address',
      m is not None and int(m.group(1)) >= 1)

f1 = free_pages(len(buf))
check('pmm steady across the run',
      f0 is not None and f1 is not None and abs(f1 - f0) <= 4)

qemu.kill()
print('FAILURES: %s' % (', '.join(fails) if fails else 'none'))
print('ALL OK' if not fails else 'SOME FAILED')
sys.exit(1 if fails else 0)
