#!/usr/bin/env python3
# M76 acceptance: real-stack interop. The guest boots behind
# QEMU's user-net (slirp) with the same legacy virtio-net;
# the peer is no longer a script playing packets but the
# host's real TCP stack -- a python echo server for the
# outbound direction, and a host-driven connection through a
# hostfwd into busybox nc listening in the guest.
import socket, subprocess, sys, time, re, os, threading

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(REPO)
SER = '/tmp/serSLIRP'
try:
    os.unlink(SER)
except FileNotFoundError:
    pass

fails = []

def check(name, cond):
    print(('PASS ' if cond else 'FAIL ') + name)
    if not cond:
        fails.append(name)

# --- the host echo server (outbound target) -----------------
echo_got = []
def echo_server():
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(('127.0.0.1', 19152))
    ls.listen(1)
    ls.settimeout(60)
    try:
        c, _ = ls.accept()
    except socket.timeout:
        return
    c.settimeout(30)
    data = b''
    while b'\n' not in data:
        chunk = c.recv(4096)
        if not chunk:
            break
        data += chunk
    echo_got.append(data)
    c.sendall(b'echo:' + data)
    time.sleep(0.3)
    c.close()
    ls.close()

threading.Thread(target=echo_server, daemon=True).start()

qemu = subprocess.Popen([
    'qemu-system-x86_64', '-m', '128',
    '-kernel', 'build/yousj-stub.elf',
    '-initrd', 'build/yousj-kernel.elf',
    '-display', 'none',
    '-serial', 'unix:%s,server,nowait' % SER,
    '-monitor', 'none', '-no-reboot',
    '-netdev', 'user,id=n0,hostfwd=tcp::18151-:8151',
    '-object', 'filter-dump,id=f0,netdev=n0,file=build/slirp_suite.pcap',
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

def until(needle, timeout=40, start=0):
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

check('link up on slirp', until('net: link up', 45))
check('shell prompt', until('sh> ', 45))
f0 = free_pages(len(buf))

def tcp_estab(mark):
    send('nettest')
    if not until('NETTEST DONE', 30, mark):
        return None
    seg = buf[mark:].decode(errors='replace')
    m = re.search(r'tcp_estab=(\d+)', seg)
    return int(m.group(1)) if m else None

estab0 = tcp_estab(len(buf))

# 1. Outbound: the guest's busybox nc talks to the real host
# echo server through slirp (guest sees it at 10.0.2.2). The
# input rides a file redirect, not a pipe: shell pipelines
# around busybox are under separate investigation (child
# contexts in back-to-back spawns corrupt intermittently),
# and the pipe is not what this suite is about.
mark = len(buf)
send('busybox echo ping-from-guest > /xf')
check('payload file written', until('sh> ', 40, mark))
send('busybox nc -w 5 10.0.2.2 19152 < /xf')
check('guest nc got the echo',
      until('echo:ping-from-guest', 40, mark))
check('guest shell back', until('sh> ', 40, mark))
check('host heard the guest',
      len(echo_got) == 1 and echo_got[0] == b'ping-from-guest\n')
estab1 = tcp_estab(len(buf))
check('exactly one outbound connection',
      estab0 is not None and estab1 == estab0 + 1)
estab0 = estab1

# 2. Inbound: busybox nc listens in the guest, the host
# connects through the hostfwd and gets cat echo back.
send('busybox nc -l -p 8151 -e /bin/cat')
mark_in = len(buf)
time.sleep(1.0)
hs = None
for _ in range(30):
    try:
        hs = socket.create_connection(('127.0.0.1', 18151),
                                      timeout=5)
        break
    except OSError:
        time.sleep(0.5)
check('host connected through the fwd', hs is not None)
got = b''
if hs is not None:
    hs.settimeout(15)
    hs.sendall(b'host says hi\n')
    try:
        while len(got) < 13:
            chunk = hs.recv(4096)
            if not chunk:
                break
            got += chunk
    except socket.timeout:
        pass
    hs.close()
check('host got cat echo from the guest', got == b'host says hi\n')


with open('build/slirp_serial_tail.log', 'w') as fh:
    fh.write(buf[-6000:].decode(errors='replace'))
print('ALL OK' if not fails else 'FAILURES PRESENT')
qemu.terminate()
sys.exit(0 if not fails else 1)
